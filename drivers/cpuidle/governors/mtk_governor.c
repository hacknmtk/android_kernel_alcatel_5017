
#include <linux/kernel.h>
#include <linux/cpuidle.h>
#include <linux/pm_qos.h>
#include <linux/time.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/sched.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/cpu.h>

#include <mach/mt_cpufreq.h>
#include <mach/mt_clkmgr.h>
#include "../mtk_cpuidle_internal.h"

#define BUCKETS 12
#define INTERVALS 8
#define RESOLUTION 1024
#define DECAY 8
#define MAX_INTERESTING 50000
#define STDDEV_THRESH 400

#define MTK_IDLE_GOVERNOR


struct mtk_idle_device {
    unsigned int        cpu;
    int                 last_state_idx;
};

struct menu_device {
	int		last_state_idx;
	int             needs_update;

	unsigned int	expected_us;
	u64		predicted_us;
	unsigned int	exit_us;
	unsigned int	bucket;
	u64		correction_factor[BUCKETS];
	u32		intervals[INTERVALS];
	int		interval_ptr;
};

#define LOAD_INT(x) ((x) >> FSHIFT)
#define LOAD_FRAC(x) LOAD_INT(((x) & (FIXED_1-1)) * 100)

static int get_loadavg(void)
{
	unsigned long this = this_cpu_load();


	return LOAD_INT(this) * 10 + LOAD_FRAC(this) / 10;
}

static inline int which_bucket(unsigned int duration)
{
	int bucket = 0;

	/*
	 * We keep two groups of stats; one with no
	 * IO pending, one without.
	 * This allows us to calculate
	 * E(duration)|iowait
	 */
	if (nr_iowait_cpu(smp_processor_id()))
		bucket = BUCKETS/2;

	if (duration < 10)
		return bucket;
	if (duration < 100)
		return bucket + 1;
	if (duration < 1000)
		return bucket + 2;
	if (duration < 10000)
		return bucket + 3;
	if (duration < 100000)
		return bucket + 4;
	return bucket + 5;
}

static inline int performance_multiplier(void)
{
	int mult = 1;

	/* for higher loadavg, we are more reluctant */

	/*
	 * this doesn't work as intended - it is almost always 0, but can
	 * sometimes, depending on workload, spike very high into the hundreds
	 * even when the average cpu load is under 10%.
	 */
	/* mult += 2 * get_loadavg(); */

	/* for IO wait tasks (per cpu!) we add 5x each */
	mult += 10 * nr_iowait_cpu(smp_processor_id());

	return mult;
}

#ifdef MTK_IDLE_GOVERNOR

#define IDLE_TAG     "[Power/swap]"
#define idle_ver(fmt, args...)		pr_info(IDLE_TAG fmt, ##args)	/* pr_debug show nothing */

extern unsigned long localtimer_get_counter(void);
extern bool clkmgr_idle_can_enter(unsigned int *condition_mask, unsigned int *block_mask);

static DEFINE_PER_CPU(struct mtk_idle_device, mtk_idle_devices);

static unsigned int     dpidle_timer_left;
static unsigned int     dpidle_timer_left2;

static unsigned long long   dpidle_block_prev_time = 0;
static unsigned int 	    idle_spm_lock = 0;

static long int idle_get_current_time_ms(void)
{
    struct timeval t;
    do_gettimeofday(&t);
    return ((t.tv_sec & 0xFFF) * 1000000 + t.tv_usec) / 1000;
}

static DEFINE_SPINLOCK(idle_spm_spin_lock);

void idle_lock_spm(enum idle_lock_spm_id id)
{
    unsigned long flags;
    spin_lock_irqsave(&idle_spm_spin_lock, flags);
    idle_spm_lock |= (1 << id);
    spin_unlock_irqrestore(&idle_spm_spin_lock, flags);
}

void idle_unlock_spm(enum idle_lock_spm_id id)
{
    unsigned long flags;
    spin_lock_irqsave(&idle_spm_spin_lock, flags);
    idle_spm_lock &= ~(1 << id);
    spin_unlock_irqrestore(&idle_spm_spin_lock, flags);
}


static bool dpidle_can_enter(void)
{
    bool ret = false;
    int reason = NR_REASONS;
    int i = 0;
    unsigned long long dpidle_block_curr_time = 0;

    if (dpidle_by_pass_cg == false) {
        if (!mt_cpufreq_earlysuspend_status_get()){
            reason = BY_VTG;
            goto out;
        }
    }

#ifdef CONFIG_SMP
    if ((atomic_read(&is_in_hotplug) >= 1) 
            || (num_online_cpus() != 1)) {
        reason = BY_CPU;
        goto out;
    }
#endif

    if (idle_spm_lock) {
        reason = BY_VTG;
        goto out;
    }

    if (dpidle_by_pass_cg == false) {
	    memset(dpidle_block_mask, 0, NR_GRPS * sizeof(unsigned int));
	    if (!clkmgr_idle_can_enter(dpidle_condition_mask, dpidle_block_mask)) {
	        reason = BY_CLK;
	        goto out;
	    }
    }

#ifdef CONFIG_SMP
    dpidle_timer_left = localtimer_get_counter();
    if ((int)dpidle_timer_left < dpidle_time_critera ||
            ((int)dpidle_timer_left) < 0) {
        reason = BY_TMR;
        goto out;
    }
#else
    gpt_get_cnt(GPT1, &dpidle_timer_left);
    gpt_get_cmp(GPT1, &dpidle_timer_cmp);
    if ((dpidle_timer_cmp - dpidle_timer_left) < dpidle_time_critera)
    {
        reason = BY_TMR;
        goto out;
    }
#endif

out:
    if (reason < NR_REASONS) {
        if (dpidle_block_prev_time == 0)
            dpidle_block_prev_time = idle_get_current_time_ms();

        dpidle_block_curr_time = idle_get_current_time_ms();
        if ((dpidle_block_curr_time - dpidle_block_prev_time) > dpidle_block_time_critera) {
            if (smp_processor_id() == 0) {
                for (i = 0; i < nr_cpu_ids; i++) {
                    idle_ver("dpidle_cnt[%d]=%lu, rgidle_cnt[%d]=%lu\n",
                            i, idle_cnt_get(IDLE_TYPE_DP, i), i, idle_cnt_get(IDLE_TYPE_RG, i));
                }

                for (i = 0; i < NR_REASONS; i++) {
                    idle_ver("[%d]dpidle_block_cnt[%s]=%lu\n", i, reason_name[i],
                            idle_block_cnt_get(IDLE_TYPE_DP, i));
                }

                for (i = 0; i < NR_GRPS; i++) {
                    idle_ver("[%02d]dpidle_condition_mask[%-8s]=0x%08x\t\t"
                            "dpidle_block_mask[%-8s]=0x%08x\n", i,
                            grp_get_name(i), dpidle_condition_mask[i],
                            grp_get_name(i), dpidle_block_mask[i]);
                }
                idle_block_cnt_clr(IDLE_TYPE_DP);
                dpidle_block_prev_time = idle_get_current_time_ms();
            }
        }
        idle_block_cnt_inc(IDLE_TYPE_DP, reason);
        ret = false;
    }
    else {
        dpidle_block_prev_time = idle_get_current_time_ms();
        ret = true;
    }

    return ret;
}

static inline int dpidle_handler(int cpu)
{
    int ret = 0;

    if (idle_switch_get(IDLE_TYPE_DP)) {
        if (dpidle_can_enter()) {
            ret = 1;
        }
    }

    return ret;
}

static inline int soidle_handler(int cpu)
{
    int ret = 0;

    /* TODO */

    return ret;
}

static inline int slidle_handler(int cpu)
{
    int ret = 0;

    /* TODO */

    return ret;
}

static inline int rgidle_handler(int cpu)
{
    int ret = 0;
    if (idle_switch_get(IDLE_TYPE_RG)) {
        ret = 1;
    }

    return ret;
}

static int (*idle_state_handler[NR_TYPES])(int) = {
    dpidle_handler,
    soidle_handler,
    slidle_handler,
    rgidle_handler,
};

static int mtk_governor_select(struct cpuidle_driver *drv, struct cpuidle_device *dev)
{
	struct mtk_idle_device *data = &__get_cpu_var(mtk_idle_devices);
    int i;

    for (i = 0; i < NR_TYPES; i++) {
        if (idle_state_handler[i](data->cpu))
            break;
    }

    return i;
}
#else
static DEFINE_PER_CPU(struct menu_device, menu_devices);

static void menu_update(struct cpuidle_driver *drv, struct cpuidle_device *dev);

/* This implements DIV_ROUND_CLOSEST but avoids 64 bit division */
static u64 div_round64(u64 dividend, u32 divisor)
{
	return div_u64(dividend + (divisor / 2), divisor);
}

static void get_typical_interval(struct menu_device *data)
{
	int i = 0, divisor = 0;
	uint64_t max = 0, avg = 0, stddev = 0;
	int64_t thresh = LLONG_MAX; /* Discard outliers above this value. */

again:

	/* first calculate average and standard deviation of the past */
	max = avg = divisor = stddev = 0;
	for (i = 0; i < INTERVALS; i++) {
		int64_t value = data->intervals[i];
		if (value <= thresh) {
			avg += value;
			divisor++;
			if (value > max)
				max = value;
		}
	}
	do_div(avg, divisor);

	for (i = 0; i < INTERVALS; i++) {
		int64_t value = data->intervals[i];
		if (value <= thresh) {
			int64_t diff = value - avg;
			stddev += diff * diff;
		}
	}
	do_div(stddev, divisor);
	stddev = int_sqrt(stddev);
	/*
	 * If we have outliers to the upside in our distribution, discard
	 * those by setting the threshold to exclude these outliers, then
	 * calculate the average and standard deviation again. Once we get
	 * down to the bottom 3/4 of our samples, stop excluding samples.
	 *
	 * This can deal with workloads that have long pauses interspersed
	 * with sporadic activity with a bunch of short pauses.
	 *
	 * The typical interval is obtained when standard deviation is small
	 * or standard deviation is small compared to the average interval.
	 */
	if (((avg > stddev * 6) && (divisor * 4 >= INTERVALS * 3))
							|| stddev <= 20) {
		data->predicted_us = avg;
		return;

	} else if ((divisor * 4) > INTERVALS * 3) {
		/* Exclude the max interval */
		thresh = max - 1;
		goto again;
	}
}

static int mtk_governor_select(struct cpuidle_driver *drv, struct cpuidle_device *dev)
{
    /* TODO: implement actions for MTK governor & replace it */
	struct menu_device *data = &__get_cpu_var(menu_devices);
	int latency_req = pm_qos_request(PM_QOS_CPU_DMA_LATENCY);
	int i;
	int multiplier;
	struct timespec t;

	if (data->needs_update) {
		menu_update(drv, dev);
		data->needs_update = 0;
	}

	data->last_state_idx = 0;
	data->exit_us = 0;

	/* Special case when user has set very strict latency requirement */
	if (unlikely(latency_req == 0))
		return 0;

	/* determine the expected residency time, round up */
	t = ktime_to_timespec(tick_nohz_get_sleep_length());
	data->expected_us =
		t.tv_sec * USEC_PER_SEC + t.tv_nsec / NSEC_PER_USEC;


	data->bucket = which_bucket(data->expected_us);

	multiplier = performance_multiplier();

	/*
	 * if the correction factor is 0 (eg first time init or cpu hotplug
	 * etc), we actually want to start out with a unity factor.
	 */
	if (data->correction_factor[data->bucket] == 0)
		data->correction_factor[data->bucket] = RESOLUTION * DECAY;

	/* Make sure to round up for half microseconds */
	data->predicted_us = div_round64(data->expected_us * data->correction_factor[data->bucket],
					 RESOLUTION * DECAY);

	get_typical_interval(data);

	/*
	 * We want to default to C1 (hlt), not to busy polling
	 * unless the timer is happening really really soon.
	 */
	if (data->expected_us > 5 &&
	    !drv->states[CPUIDLE_DRIVER_STATE_START].disabled &&
		dev->states_usage[CPUIDLE_DRIVER_STATE_START].disable == 0)
		data->last_state_idx = CPUIDLE_DRIVER_STATE_START;

	/*
	 * Find the idle state with the lowest power while satisfying
	 * our constraints.
	 */
	for (i = CPUIDLE_DRIVER_STATE_START; i < drv->state_count; i++) {
		struct cpuidle_state *s = &drv->states[i];
		struct cpuidle_state_usage *su = &dev->states_usage[i];
///*
        if (printk_ratelimit()) {
            printk(KERN_WARNING "[cpuidle] ### data->expected_us = %u, data->correction_factor[data->bucket] = %llu\n", data->expected_us, data->correction_factor[data->bucket]);
            printk(KERN_WARNING "[cpuidle] s->disabled = %d, su->disable = %llu\n", s->disabled, su->disable);
            printk(KERN_WARNING "[cpuidle] s->target_residency = %d, su->predicted_us = %llu\n", s->target_residency, data->predicted_us);
            printk(KERN_WARNING "[cpuidle] exit_latency = %d, latency_req = %d\n", s->exit_latency, latency_req);
            printk(KERN_WARNING "[cpuidle] exit_latency * multiplier = %d, predicted_us = %llu\n", s->exit_latency * multiplier, data->predicted_us);
        }
//*/
		if (s->disabled || su->disable)
			continue;
		if (s->target_residency > data->predicted_us)
			continue;
		if (s->exit_latency > latency_req) {
			continue;
        }
		if (s->exit_latency * multiplier > data->predicted_us)
			continue;
		data->last_state_idx = i;
		data->exit_us = s->exit_latency;
	}

	return data->last_state_idx;
}
#endif
static void mtk_governor_reflect(struct cpuidle_device *dev, int index)
{
    /* TODO: implement actions for MTK governor & replace it */
}

#ifndef MTK_IDLE_GOVERNOR
static void menu_update(struct cpuidle_driver *drv, struct cpuidle_device *dev)
{
	struct menu_device *data = &__get_cpu_var(menu_devices);
	int last_idx = data->last_state_idx;
	unsigned int last_idle_us = cpuidle_get_last_residency(dev);
	struct cpuidle_state *target = &drv->states[last_idx];
	unsigned int measured_us;
	u64 new_factor;

	/*
	 * Ugh, this idle state doesn't support residency measurements, so we
	 * are basically lost in the dark.  As a compromise, assume we slept
	 * for the whole expected time.
	 */
	if (unlikely(!(target->flags & CPUIDLE_FLAG_TIME_VALID)))
		last_idle_us = data->expected_us;


	measured_us = last_idle_us;

	/*
	 * We correct for the exit latency; we are assuming here that the
	 * exit latency happens after the event that we're interested in.
	 */
	if (measured_us > data->exit_us)
		measured_us -= data->exit_us;


	/* update our correction ratio */

	new_factor = data->correction_factor[data->bucket]
			* (DECAY - 1) / DECAY;

	if (data->expected_us > 0 && measured_us < MAX_INTERESTING)
		new_factor += RESOLUTION * measured_us / data->expected_us;
	else
		/*
		 * we were idle so long that we count it as a perfect
		 * prediction
		 */
		new_factor += RESOLUTION;

	/*
	 * We don't want 0 as factor; we always want at least
	 * a tiny bit of estimated time.
	 */
	if (new_factor == 0)
		new_factor = 1;

	data->correction_factor[data->bucket] = new_factor;

	/* update the repeating-pattern data */
	data->intervals[data->interval_ptr++] = last_idle_us;
	if (data->interval_ptr >= INTERVALS)
		data->interval_ptr = 0;
}
#endif

static int mtk_governor_enable_device(struct cpuidle_driver *drv,
				struct cpuidle_device *dev)
{
    /* TODO: implement actions for MTK governor & replace it */
#ifdef MTK_IDLE_GOVERNOR
    struct mtk_idle_device *data = &per_cpu(mtk_idle_devices, dev->cpu);

    memset(data, 0, sizeof(struct mtk_idle_device));
    data->cpu = dev->cpu;

    return 0;
#else
	struct menu_device *data = &per_cpu(menu_devices, dev->cpu);

	memset(data, 0, sizeof(struct menu_device));

	return 0;
#endif
}

static struct cpuidle_governor mtk_governor = {
	.name =		"mtk_governor",
	.rating =	100,
	.enable =	mtk_governor_enable_device,
	.select =	mtk_governor_select,
	.reflect =	mtk_governor_reflect,
	.owner =	THIS_MODULE,
};

static int __init init_mtk_governor(void)
{
    /* TODO: check if debugfs_create_file() failed */
    mtk_cpuidle_debugfs_init();
	return cpuidle_register_governor(&mtk_governor);
}

static void __exit exit_mtk_governor(void)
{
	cpuidle_unregister_governor(&mtk_governor);
}

MODULE_LICENSE("GPL");
module_init(init_mtk_governor);
module_exit(exit_mtk_governor);
