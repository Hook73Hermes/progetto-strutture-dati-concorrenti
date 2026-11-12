#!/bin/bash

if [ "$EUID" -ne 0 ]; then
    echo "Run as root: sudo $0" >&2
    exit 1
fi

# Ripristina le affinita' degli interrupt salvate da setup.sh (in mancanza, tutte le cpu)
saved=/run/pubsub-irq-affinity
if [ -e "$saved" ]; then
    while read -r irq aff; do
        if [ "$irq" = default ]; then
            echo "$aff" > /proc/irq/default_smp_affinity
        else
            echo "$aff" > "/proc/irq/$irq/smp_affinity_list" 2>/dev/null
        fi
    done < "$saved"
    rm -f "$saved"
else
    echo ff > /proc/irq/default_smp_affinity
    for irq in /proc/irq/[0-9]*; do
        echo 0-7 > "$irq/smp_affinity_list" 2>/dev/null
    done
fi

# Riattiva il turbo boost (prima di leggere cpuinfo_max_freq)
echo 0 > /sys/devices/system/cpu/intel_pstate/no_turbo

# Ripristina il range di frequenze e il governor di default
for p in /sys/devices/system/cpu/cpufreq/policy*; do
    cat "$p/cpuinfo_min_freq" > "$p/scaling_min_freq"
    cat "$p/cpuinfo_max_freq" > "$p/scaling_max_freq"
    echo powersave > "$p/scaling_governor"
    echo balance_performance > "$p/energy_performance_preference"
done

# Riabilita tutti gli idle state
for s in /sys/devices/system/cpu/cpu*/cpuidle/state*; do
    echo 0 > "$s/disable"
done
