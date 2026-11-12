#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/ioctl.h>
#include "pubsub_ioctl.h"

#define CTRL_DEV      "/dev/pubsub_ctrl"
#define TOPIC         "bench"
#define TOPIC_DEV     "/dev/pubsub/" TOPIC
#define VARIANT_FILE  "/sys/module/pubsub/parameters/variant"

#define WARMUP_S      1             // Secondi di riscaldamento, scartati
#define DURATION_S    5             // Secondi di misura
#define MSG_SIZE      16            // Byte per write() (il modulo non conserva il payload)

// Cpu di misura: prima i 4 core fisici, poi i sibling 5-7 delle cpu 1-3. La cpu 4 e' la cpu di servizio
// I publisher prendono le prime P cpu della lista, i subscriber le S successive.
#define MAX_THREADS   7
#define SERVICE_CPU   4
static const int measure_cpus[MAX_THREADS] = { 0, 1, 2, 3, 5, 6, 7 };

// Dati di un thread, allineati a 64 byte (cacheline)
struct thread_data {
    atomic_ulong count;             // Messaggi scritti (publisher) o letti (subscriber), scritto solo dal thread
    unsigned int dropped;           // Messaggi persi (solo subscriber, letto a fine run con GET_DROPPED)
    int cpu;                        // Cpu su cui e' fissato il thread
    int fd;                         // File descriptor sul device del topic
    int err;                        // errno del primo errore (0 se nessun errore)
} __attribute__((aligned(64)));

static struct thread_data pubs[MAX_THREADS];
static struct thread_data subs[MAX_THREADS];
static pthread_t pub_threads[MAX_THREADS];
static pthread_t sub_threads[MAX_THREADS];

static pthread_barrier_t start_barrier;
static atomic_int stop;
static int ctrl_fd = -1;

// Esegue CREATE/DESTROY sul device di controllo
static int topic_cmd(unsigned long cmd, const char *name)
{
    struct pubsub_topic_req req;

    memset(&req, 0, sizeof(req));
    strncpy(req.name, name, PUBSUB_MAX_NAME_LEN - 1);
    return ioctl(ctrl_fd, cmd, &req) == 0 ? 0 : -errno;
}

// Errore non recuperabile: distrugge il topic (sblocca eventuali subscriber) ed esce
static void fatal(const char *msg)
{
    perror(msg);
    if (ctrl_fd >= 0)
        topic_cmd(PUBSUB_DESTROY_TOPIC, TOPIC);
    exit(1);
}

// Converte una stringa in intero >= 0; -1 se non valida
static long parse_count(const char *s)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 0)
        return -1;
    return v;
}

// Legge la variante del modulo caricato (rwlock o rcu)
static void read_variant(char *buf, size_t len)
{
    FILE *f = fopen(VARIANT_FILE, "r");

    if (!f || !fgets(buf, len, f))
        fatal(VARIANT_FILE " (modulo pubsub caricato?)");
    fclose(f);
    buf[strcspn(buf, "\n")] = '\0';
}

// Istante corrente in nanosecondi (CLOCK_MONOTONIC)
static long long now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Apre il device del topic, aspettando fino a 1 s che il nodo compaia in /dev
static int open_topic(int flags)
{
    int fd;

    for (int i = 0; i < 100; i++) {
        fd = open(TOPIC_DEV, flags);
        if (fd >= 0 || errno != ENOENT)
            return fd;
        usleep(10000);
    }
    return -1;
}

// Publisher: write() in ciclo finche' main non alza stop
static void *publisher_body(void *arg)
{
    struct thread_data *td = arg;
    char msg[MSG_SIZE] = { 0 };
    unsigned long n = 0;

    pthread_barrier_wait(&start_barrier);

    while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
        if (write(td->fd, msg, sizeof(msg)) != (ssize_t)sizeof(msg)) {
            td->err = errno;
            break;
        }
        n++;
        atomic_store_explicit(&td->count, n, memory_order_relaxed);
    }
    return NULL;
}

// Subscriber: read() bloccante in ciclo finche' il topic viene distrutto (ENODEV a coda vuota)
static void *subscriber_body(void *arg)
{
    struct thread_data *td = arg;
    unsigned long seq, n = 0;
    ssize_t r;

    pthread_barrier_wait(&start_barrier);

    for (;;) {
        r = read(td->fd, &seq, sizeof(seq));
        if (r == (ssize_t)sizeof(seq)) {
            n++;
            atomic_store_explicit(&td->count, n, memory_order_relaxed);
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0 && errno == ENODEV)
            break;
        td->err = r < 0 ? errno : EIO;
        break;
    }

    if (ioctl(td->fd, PUBSUB_GET_DROPPED, &td->dropped) < 0 && !td->err)
        td->err = errno;
    return NULL;
}

// Crea un thread fissato sulla cpu td->cpu (affinita' impostata prima della creazione)
static void start_thread(pthread_t *th, void *(*body)(void *), struct thread_data *td)
{
    pthread_attr_t attr;
    cpu_set_t set;
    int ret;

    CPU_ZERO(&set);
    CPU_SET(td->cpu, &set);
    pthread_attr_init(&attr);
    pthread_attr_setaffinity_np(&attr, sizeof(set), &set);
    ret = pthread_create(th, &attr, body, td);
    pthread_attr_destroy(&attr);
    if (ret != 0) {
        errno = ret;
        fatal("pthread_create");
    }
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s -p <publisher >= 1> -s <subscriber >= 0>   (p + s <= %d)\n", prog, MAX_THREADS);
    exit(1);
}

int main(int argc, char *argv[])
{
    char variant[32];
    long p = -1, s = -1;
    unsigned long c0[MAX_THREADS], c1[MAX_THREADS];
    long long t0, t1;
    double ns_write = 0;
    cpu_set_t set;
    int opt, ret, failed = 0;

    while ((opt = getopt(argc, argv, "p:s:")) != -1) {
        switch (opt) {
        case 'p': p = parse_count(optarg); break;
        case 's': s = parse_count(optarg); break;
        default:  usage(argv[0]);
        }
    }
    if (optind != argc || p < 1 || s < 0 || p + s > MAX_THREADS)
        usage(argv[0]);

    read_variant(variant, sizeof(variant));

    // Il thread main resta sulla cpu di servizio
    CPU_ZERO(&set);
    CPU_SET(SERVICE_CPU, &set);
    if (sched_setaffinity(0, sizeof(set), &set) < 0)
        fatal("sched_setaffinity");

    ctrl_fd = open(CTRL_DEV, O_RDWR);
    if (ctrl_fd < 0)
        fatal(CTRL_DEV);

    // Elimina un topic eventualmente rimasto
    topic_cmd(PUBSUB_DESTROY_TOPIC, TOPIC);
    ret = topic_cmd(PUBSUB_CREATE_TOPIC, TOPIC);
    if (ret < 0) {
        errno = -ret;
        fatal("create topic " TOPIC);
    }

    // I descrittori si aprono tutti prima di partire
    for (int i = 0; i < s; i++) {
        subs[i].cpu = measure_cpus[p + i];
        subs[i].fd = open_topic(O_RDONLY);
        if (subs[i].fd < 0)
            fatal("open subscriber " TOPIC_DEV);
    }
    for (int i = 0; i < p; i++) {
        pubs[i].cpu = measure_cpus[i];
        pubs[i].fd = open_topic(O_WRONLY);
        if (pubs[i].fd < 0)
            fatal("open publisher " TOPIC_DEV);
    }

    printf("# variant %s, P %ld, S %ld, warmup %d s, durata %d s\n", variant, p, s, WARMUP_S, DURATION_S);
    printf("# cpu: pub=");
    for (int i = 0; i < p; i++)
        printf("%s%d", i ? "," : "", pubs[i].cpu);
    printf(" sub=");
    for (int i = 0; i < s; i++)
        printf("%s%d", i ? "," : "", subs[i].cpu);
    printf("%s\n", s ? "" : "-");
    fflush(stdout);

    pthread_barrier_init(&start_barrier, NULL, p + s + 1);
    for (int i = 0; i < s; i++)
        start_thread(&sub_threads[i], subscriber_body, &subs[i]);
    for (int i = 0; i < p; i++)
        start_thread(&pub_threads[i], publisher_body, &pubs[i]);

    // Partenza comune di tutti i thread
    pthread_barrier_wait(&start_barrier);

    // Contano solo le differenze c1 - c0 e t1 - t0
    sleep(WARMUP_S);
    t0 = now_ns();
    for (int i = 0; i < p; i++)
        c0[i] = atomic_load_explicit(&pubs[i].count, memory_order_relaxed);
    sleep(DURATION_S);
    t1 = now_ns();
    for (int i = 0; i < p; i++)
        c1[i] = atomic_load_explicit(&pubs[i].count, memory_order_relaxed);

    // Prima si fermano i publisher, poi la destroy sblocca i subscriber, che ricevono ENODEV e leggono il contatore dei messaggi persi
    atomic_store(&stop, 1);
    for (int i = 0; i < p; i++)
        pthread_join(pub_threads[i], NULL);
    if (topic_cmd(PUBSUB_DESTROY_TOPIC, TOPIC) < 0)
        fatal("destroy topic " TOPIC);
    for (int i = 0; i < s; i++)
        pthread_join(sub_threads[i], NULL);

    for (int i = 0; i < p; i++) {
        printf("# pub %d: %lu write%s%s\n", i, atomic_load(&pubs[i].count),
               pubs[i].err ? ", errore: " : "", pubs[i].err ? strerror(pubs[i].err) : "");
        failed |= pubs[i].err != 0;
        close(pubs[i].fd);
    }
    for (int i = 0; i < s; i++) {
        printf("# sub %d: %lu letti, %u persi%s%s\n", i, atomic_load(&subs[i].count), subs[i].dropped,
               subs[i].err ? ", errore: " : "", subs[i].err ? strerror(subs[i].err) : "");
        failed |= subs[i].err != 0;
        close(subs[i].fd);
    }

    pthread_barrier_destroy(&start_barrier);
    close(ctrl_fd);
    printf("# chiusura completata%s\n", failed ? " (CON ERRORI)" : "");

    // ns/write: costo medio di una write() per publisher nella finestra, media fra i publisher
    for (int i = 0; i < p; i++) {
        if (c1[i] == c0[i]) {
            printf("# pub %d: nessuna write\n", i);
            failed = 1;
            continue;
        }
        ns_write += (double)(t1 - t0) / (c1[i] - c0[i]) / p;
    }

    // Riga di dati (senza #), raccolta dagli script
    printf("# variant P S ns_write\n");
    printf("%s %ld %ld %.1f\n", variant, p, s, ns_write);
    return failed ? 1 : 0;
}
