#!/bin/bash

set -e

if [ "$EUID" -ne 0 ]; then
    echo "Run as root: sudo $0" >&2
    exit 1
fi

# Compila come l'utente che ha lanciato sudo, cosi' i file in build/ non diventano di root
as_user() {
    sudo -u "${SUDO_USER:-root}" "$@"
}

as_user make user > /dev/null

as_user make kernel RCU=y > /dev/null
rmmod pubsub 2>/dev/null || true
insmod build/pubsub.ko
echo -e "- Misure con variante RCU, con numero di produttori e consumatori variabile\n"
./run.sh -p 1 -s 0
echo ""
./run.sh -p 2 -s 0
echo ""
./run.sh -p 4 -s 0
echo ""
./run.sh -p 2 -s 1
echo ""
./run.sh -p 4 -s 3
echo ""
rmmod pubsub

as_user make kernel > /dev/null
rmmod pubsub 2>/dev/null || true
insmod build/pubsub.ko
echo -e "- Misure con variante RWLOCK, con numero di produttori e consumatori variabile\n"
./run.sh -p 1 -s 0
echo ""
./run.sh -p 2 -s 0
echo ""
./run.sh -p 4 -s 0
echo ""
./run.sh -p 2 -s 1
echo ""
./run.sh -p 4 -s 3
rmmod pubsub