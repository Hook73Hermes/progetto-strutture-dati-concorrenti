# Meccanismo di IPC pub/sub in-kernel

Modulo kernel Linux che implementa un meccanismo di comunicazione publish/subscribe tra processi userspace tramite device file (`open`, `read`, `write`, `poll`, `ioctl`), supportando comunicazione 1-a-1, 1-a-molti, molti-a-1 e molti-a-molti.

Un device di controllo (`/dev/pubsub_ctrl`) permette di creare/eliminare topic. Ogni topic espone un proprio device (`/dev/pubsub/<topic>`) su cui i processi si registrano come publisher (scrittura) o subscriber (lettura). Le scritture non sono mai bloccanti, le letture lo sono di default.

Il progetto confronta sperimentalmente due strategie di sincronizzazione per l'elenco dei subscriber (**RCU vs rwlock_t**), con l'idea di misurare throughput e latenza al variare del numero di publisher/subscriber. La variante si sceglie in compilazione: `make` (rwlock) oppure `make RCU=y` (RCU).

**File**:
- `pubsub.c`: modulo kernel.
- `pubsub_ioctl.h`: definizioni delle ioctl, condiviso tra modulo e programmi userspace.
- `pubsub_benchmark.c`: benchmark userspace (`-p` publisher, `-s` subscriber), stampa i ns per `write()`.
- `Makefile`: `make kernel [RCU=y]` compila il modulo, `make user` il benchmark, `make` entrambi.
- `setup.sh` / `reset.sh`: configurano la macchina per misure riproducibili (frequenza fissa, niente turbo, interrupt sulla cpu 4) e ripristinano i default.
- `run.sh`: esegue `setup.sh`, il benchmark con gli argomenti ricevuti e infine `reset.sh`.
- `test.sh`: compila e carica il modulo in variante RCU e poi rwlock, ed esegue per ciascuna i run del benchmark.