#!/bin/bash

set -e

if [ "$EUID" -ne 0 ]; then
    echo "Run as root: sudo $0" >&2
    exit 1
fi

# Disattiva il turbo boost (prima di leggere cpuinfo_max_freq)
echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo

# Fissa le frequenze delle cpu per rendere i test riproducibili
for p in /sys/devices/system/cpu/cpufreq/policy*; do
    echo performance > "$p/scaling_governor"
    cat "$p/cpuinfo_max_freq" > "$p/scaling_max_freq"
    cat "$p/cpuinfo_max_freq" > "$p/scaling_min_freq"
done

# Lascia abilitato solo l'idle state minore (state0), disabilita gli altri
for s in /sys/devices/system/cpu/cpu*/cpuidle/state[1-9]*; do
    echo 1 > "$s/disable"
done

# Salva le affinita' originali degli interrupt in /run (tmpfs) per reset.sh, solo se non gia' salvate
saved=/run/pubsub-irq-affinity
if [ ! -e "$saved" ]; then
    echo "default $(cat /proc/irq/default_smp_affinity)" > "$saved"
    for irq in /proc/irq/[0-9]*; do
        echo "$(basename "$irq") $(cat "$irq/smp_affinity_list")" >> "$saved"
    done
fi

# Manda tutti gli interrupt spostabili sulla cpu 4 (maschera 10), lontano dalle cpu di misura.
echo 10 > /proc/irq/default_smp_affinity
not_moved=""
for irq in /proc/irq/[0-9]*; do
    echo 4 > "$irq/smp_affinity_list" 2>/dev/null || not_moved="$not_moved $(basename "$irq")"
done
[ -n "$not_moved" ] && echo "IRQ non spostabili:$not_moved" >&2
true
