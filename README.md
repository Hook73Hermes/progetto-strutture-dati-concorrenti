# Meccanismo di IPC pub/sub in-kernel

Modulo kernel Linux che implementa un meccanismo di comunicazione publish/subscribe tra processi userspace tramite device file (`open`, `read`, `write`, `poll`, `ioctl`), supportando comunicazione 1-a-1, 1-a-molti, molti-a-1 e molti-a-molti.

Un device di controllo (`/dev/pubsub`) permette di creare/eliminare topic. Ogni topic espone un proprio device (`/dev/pubsub/<topic>`) su cui i processi si registrano come publisher (scrittura) o subscriber (lettura). Le scritture non sono mai bloccanti, le letture lo sono di default.

Il progetto confronta sperimentalmente due strategie di sincronizzazione per l'elenco dei subscriber (**RCU vs rwlock_t**), misurando throughput e latenza al variare del numero di publisher/subscriber.

**File**: `pubsub.c` (modulo kernel), `pubsub_test.c` (benchmark userspace), `pubsub_ioctl.h` (gestione ioctl, incluso dagli altri file C), `Makefile`.