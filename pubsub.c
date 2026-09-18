#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/fs.h>
#include <linux/kref.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/idr.h>
#include <linux/rculist.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include "pubsub_ioctl.h"

#define PUBSUB_MAX_TOPICS 256
#define PUBSUB_QUEUE_DEPTH 64
#define PUBSUB_MAX_MSG_SIZE 4096

// Commentare la seguente riga per utilizzare RCU al posto dei RW-LOCK
#define PUBSUB_USE_RWLOCK

#ifdef PUBSUB_USE_RWLOCK

typedef rwlock_t subs_lock_t;
#define SUBS_LOCK_INIT(l)              rwlock_init(l)
#define SUBS_WRITE_LOCK(l)             write_lock(l)
#define SUBS_WRITE_UNLOCK(l)           write_unlock(l)
#define SUBS_READ_LOCK(l)              read_lock(l)
#define SUBS_READ_UNLOCK(l)            read_unlock(l)
#define subs_list_add(node, head)      list_add(node, head)
#define subs_list_del(node)            list_del(node)
#define subs_list_for_each(pos, head)  list_for_each_entry(pos, head, list)
#define subs_free(sub)                 kfree(sub)

#else

typedef spinlock_t subs_lock_t;
#define SUBS_LOCK_INIT(l)              spin_lock_init(l)
#define SUBS_WRITE_LOCK(l)             spin_lock(l)
#define SUBS_WRITE_UNLOCK(l)           spin_unlock(l)
#define SUBS_READ_LOCK(l)              rcu_read_lock()
#define SUBS_READ_UNLOCK(l)            rcu_read_unlock()
#define subs_list_add(node, head)      list_add_rcu(node, head)
#define subs_list_del(node)            list_del_rcu(node)
#define subs_list_for_each(pos, head)  list_for_each_entry_rcu(pos, head, list)
#define subs_free(sub)                 kfree_rcu(sub, rcu)

#endif

// Lista dei subscriber per ogni singolo topic
struct subscriber {
    struct list_head list;                  // Lista subscriber del topic
    wait_queue_head_t wq;                   // Coda di attesa per le letture (read(), poll(), delect())
    spinlock_t lock;                        // Protegge i campi della struttura
    unsigned int pending;                   // Messaggi in attesa di essere letti (contatore)
    unsigned int dropped;                   // Messaggi persi per coda piena
    unsigned long next_seq;                 // Prossimo sequence number da assegnare durante la write() successiva
    unsigned long delivered_seq;            // Ultimo sequence number consegnato (incrementato in read())
    bool dead;                              // true se il topic e' stato distrutto
    struct rcu_head rcu;                    // Usato da kfree_rcu()
};

// Lista dei topic attivi
struct topic {
    char name[PUBSUB_MAX_NAME_LEN];         // Nome del topic (univoco)
    struct list_head list;                  // Nodo nella lista globale dei topic
    dev_t dev_num;                          // Major e minor allocati per il device di questo topic
    struct cdev cdev;                       // Struttura cdev che collega dev_num a topic_fops
    struct device *device;                  // Usato solo per device_destroy()
    struct kref refcount;                   // Riferimenti attivi su questo topic
    struct list_head subscribers;           // Lista dei subscriber correnti
    subs_lock_t subs_lock;                  // Lock di scrittura sull'elenco subscriber
};

static LIST_HEAD(topic_list);
static DEFINE_MUTEX(topic_list_mutex);

// Variabili globali
static dev_t dev_num;                       // Major e minor del device di controllo
static dev_t pubsub_base_devt;              // Base major e minor allocata da alloc_chrdev_region
static struct cdev pubsub_cdev;             // Struttura cdev del device di controllo, collegata a pubsub_fops
static struct class *pubsub_class;          // Classe sysfs condivisa: usata per creare sia /dev/pubsub_ctrl che /dev/pubsub/<topic>
static DEFINE_IDA(pubsub_minor_ida);        // Allocatore dei minor number per i topic (assegna e riusa quelli liberati da un topic distrutto)

// Libera la memoria di un topic ricavando il puntatore da un campo
static void topic_free_kref(struct kref *kref)
{
    struct topic *t = container_of(kref, struct topic, refcount);
    kfree(t);
}

// Contesto di un file descriptor aperto su un topic
struct topic_fd {
    struct topic *topic;                    // Puntatore al topic associato
    struct subscriber *sub;                 // Puntatore al subscriber associato (NULL se publisher)
};

// Apre un file descriptor su un topic, inizializza il contesto e incrementa il contatore di riferimenti
static int topic_open(struct inode *inode, struct file *file)
{
    struct topic *t = container_of(inode->i_cdev, struct topic, cdev);
    struct topic_fd *tfd;

    tfd = kzalloc(sizeof(*tfd), GFP_KERNEL);
    if (!tfd) 
        return -ENOMEM;
    
    tfd->topic = t;
    tfd->sub = NULL;
    kref_get(&t->refcount);

    if ((file->f_flags & O_ACCMODE) == O_WRONLY) {
        pr_info("pubsub: publisher connesso al topic '%s'\n", t->name);
    } else if ((file->f_flags & O_ACCMODE) == O_RDONLY) {
        struct subscriber *sub = kzalloc(sizeof(*sub), GFP_KERNEL);

        if (!sub) {
            kref_put(&t->refcount, topic_free_kref);
            kfree(tfd);
            return -ENOMEM;
        }

        init_waitqueue_head(&sub->wq);
        spin_lock_init(&sub->lock);

        SUBS_WRITE_LOCK(&t->subs_lock);
        subs_list_add(&sub->list, &t->subscribers);
        SUBS_WRITE_UNLOCK(&t->subs_lock);

        tfd->sub = sub;
        pr_info("pubsub: subscriber connesso al topic '%s'\n", t->name);
    }

    file->private_data = tfd;
    return 0;
}

// Libera la memoria associata a un topic_fd
static int topic_release(struct inode *inode, struct file *file)
{
    struct topic_fd *tfd = file->private_data;
    struct topic *t = tfd->topic;

    if (tfd->sub) {
        SUBS_WRITE_LOCK(&t->subs_lock);
        subs_list_del(&tfd->sub->list);
        SUBS_WRITE_UNLOCK(&t->subs_lock);
        subs_free(tfd->sub);
    }

    kref_put(&t->refcount, topic_free_kref);
    kfree(tfd);

    return 0;
}

// Scrive un messaggio su un topic
static ssize_t topic_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    struct topic_fd *tfd = file->private_data;
    struct topic *t = tfd->topic;
    struct subscriber *sub;

    if (count > PUBSUB_MAX_MSG_SIZE) 
        return -EMSGSIZE;

    SUBS_READ_LOCK(&t->subs_lock);
    subs_list_for_each(sub, &t->subscribers) {
        unsigned long flags;
        bool delivered = false;

        spin_lock_irqsave(&sub->lock, flags);
        if (sub->pending < PUBSUB_QUEUE_DEPTH) {
            sub->pending++;
            sub->next_seq++;
            delivered = true;
        } else {
            sub->dropped++;
        }
        spin_unlock_irqrestore(&sub->lock, flags);

        if (delivered)
            wake_up_interruptible(&sub->wq);
    }
    SUBS_READ_UNLOCK(&t->subs_lock);

    return count;
}

// Legge un messaggio da un topic
static ssize_t topic_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    struct topic_fd *tfd = file->private_data;
    struct subscriber *sub = tfd->sub;
    unsigned long flags;
    unsigned long seq;
    int ret;

    if (!sub)
        return -EBADF; 

    if (count < sizeof(seq))
        return -EINVAL;

    spin_lock_irqsave(&sub->lock, flags);
    while (sub->pending == 0 && !sub->dead) {
        spin_unlock_irqrestore(&sub->lock, flags);

        if (file->f_flags & O_NONBLOCK)
            return -EAGAIN;

        ret = wait_event_interruptible(sub->wq, sub->pending > 0 || sub->dead);
        if (ret)
            return ret;

        spin_lock_irqsave(&sub->lock, flags);
    }

    if (sub->pending == 0 && sub->dead) {
        spin_unlock_irqrestore(&sub->lock, flags);
        return -ENODEV;
    }

    sub->pending--;
    sub->delivered_seq++;
    seq = sub->delivered_seq;
    spin_unlock_irqrestore(&sub->lock, flags);

    if (copy_to_user(buf, &seq, sizeof(seq)))
        return -EFAULT;

    return sizeof(seq);
}

// Gestisce le operazioni di polling per un topic
static __poll_t topic_poll(struct file *file, poll_table *wait)
{
    struct topic_fd *tfd = file->private_data;
    struct subscriber *sub = tfd->sub;
    __poll_t mask = 0;
    unsigned long flags;

    if (!sub)
        return EPOLLOUT | EPOLLWRNORM;

    poll_wait(file, &sub->wq, wait);

    spin_lock_irqsave(&sub->lock, flags);
    if (sub->pending > 0)
        mask |= EPOLLIN | EPOLLRDNORM;
    if (sub->dead)
        mask |= EPOLLIN | EPOLLHUP;
    spin_unlock_irqrestore(&sub->lock, flags);

    return mask;
}

// Gestisce le operazioni di ioctl per un topic
static long topic_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct topic_fd *tfd = file->private_data;
    struct subscriber *sub = tfd->sub;
    unsigned long flags;
    unsigned int dropped;

    if (!sub)
        return -EBADF;

    switch (cmd) {
        case PUBSUB_GET_DROPPED:
            spin_lock_irqsave(&sub->lock, flags);
            dropped = sub->dropped;
            spin_unlock_irqrestore(&sub->lock, flags);

            if (copy_to_user((unsigned int __user *)arg, &dropped, sizeof(dropped)))
                return -EFAULT;
            return 0;
        default:
            return -ENOTTY;
    }
}

// Definisce le operazioni di file per un topic
static struct file_operations topic_fops = {
    .owner = THIS_MODULE,
    .open = topic_open,
    .release = topic_release,
    .write = topic_write,
    .read = topic_read,
    .poll = topic_poll,
    .unlocked_ioctl = topic_ioctl,
};

// Crea il device per il topic
static int pubsub_topic_device_create(struct topic *t)
{
    int minor;
    int ret;

    // Alloca un minor number per il topic
    minor = ida_alloc_range(&pubsub_minor_ida, 1, PUBSUB_MAX_TOPICS, GFP_KERNEL);
    if (minor < 0)
        return minor;

    t->dev_num = MKDEV(MAJOR(pubsub_base_devt), minor);

    // Inizializza la struttura cdev per il topic
    cdev_init(&t->cdev, &topic_fops);
    t->cdev.owner = THIS_MODULE;

    // Aggiunge la struttura cdev al kernel
    ret = cdev_add(&t->cdev, t->dev_num, 1);
    if (ret < 0) {
        ida_free(&pubsub_minor_ida, minor);
        return ret;
    }

    // Crea il device per il topic
    t->device = device_create(pubsub_class, NULL, t->dev_num, NULL, "pubsub/%s", t->name);
    if (IS_ERR(t->device)) {
        ret = PTR_ERR(t->device);
        cdev_del(&t->cdev);
        ida_free(&pubsub_minor_ida, minor);
        return ret;
    }

    return 0;
}

// Distrugge il device per il topic rimuovendolo dal kernel
static void pubsub_topic_device_destroy(struct topic *t)
{
    device_destroy(pubsub_class, t->dev_num);
    cdev_del(&t->cdev);
    ida_free(&pubsub_minor_ida, MINOR(t->dev_num));
}

// Apre il device per il topic
static int pubsub_open(struct inode *inode, struct file *file)
{
    return 0;
}

// Chiude il device per il topic
static int pubsub_release(struct inode *inode, struct file *file)
{
    return 0;
}

// Crea il topic
static int pubsub_create_topic(struct pubsub_topic_req *topic_req)
{
    struct topic *t;
    int ret;

    // Verifica che il nome sia non nullo
    if (!topic_req || !topic_req->name[0])
        return -EINVAL;

    mutex_lock(&topic_list_mutex);

    // Verifica che il topic non sia presente
    list_for_each_entry(t, &topic_list, list) {
        if (strncmp(t->name, topic_req->name, PUBSUB_MAX_NAME_LEN) == 0) {
            mutex_unlock(&topic_list_mutex);
            return -EEXIST;
        }
    }

    // Alloca memoria per il topic
    t = kmalloc(sizeof(*t), GFP_KERNEL);
    if (!t) {
        mutex_unlock(&topic_list_mutex);
        return -ENOMEM;
    }

    // Copia il nome del topic
    strscpy(t->name, topic_req->name, PUBSUB_MAX_NAME_LEN);
    kref_init(&t->refcount);
    INIT_LIST_HEAD(&t->subscribers);
    SUBS_LOCK_INIT(&t->subs_lock);

    // Crea il device per il topic
    ret = pubsub_topic_device_create(t);
    if (ret < 0) {
        mutex_unlock(&topic_list_mutex);
        kfree(t);
        return ret;
    }

    // Inserisce il topic nella lista
    list_add_tail(&t->list, &topic_list);
    
    mutex_unlock(&topic_list_mutex);

    return 0;
}

// Sveglia tutti i subscriber del topic
static void topic_wake_all_subscribers(struct topic *t)
{
    struct subscriber *sub;

    SUBS_WRITE_LOCK(&t->subs_lock);
    list_for_each_entry(sub, &t->subscribers, list) {
        unsigned long flags;

        spin_lock_irqsave(&sub->lock, flags);
        sub->dead = true;
        spin_unlock_irqrestore(&sub->lock, flags);

        wake_up_interruptible(&sub->wq);
    }
    SUBS_WRITE_UNLOCK(&t->subs_lock);
}

// Distrugge il topic
static int pubsub_destroy_topic(struct pubsub_topic_req *topic_req)
{
    struct topic *t;
    
    mutex_lock(&topic_list_mutex);

    // Cerca il topic nella lista e lo elimina
    list_for_each_entry(t, &topic_list, list) {
        if (strncmp(t->name, topic_req->name, PUBSUB_MAX_NAME_LEN) == 0) {
            list_del(&t->list);
            mutex_unlock(&topic_list_mutex);
            pubsub_topic_device_destroy(t);
            topic_wake_all_subscribers(t);
            kref_put(&t->refcount, topic_free_kref);
            return 0;
        }
    }

    mutex_unlock(&topic_list_mutex);
    return -ENOENT;
}

// Elenca tutti i topic
static int pubsub_list_topics(struct pubsub_topic_list *truncated_topic_list)
{
    struct topic *t;
    int count = 0;

    mutex_lock(&topic_list_mutex);

    // Aggiunge i nomi dei topic alla lista di ritorno all'utente
    list_for_each_entry(t, &topic_list, list) {
        if (count >= PUBSUB_MAX_LISTED_TOPICS)
            break;
        strscpy(truncated_topic_list->names[count], t->name, PUBSUB_MAX_NAME_LEN);
        count++;
    }
    
    mutex_unlock(&topic_list_mutex);
    
    truncated_topic_list->count = count;

    return 0;
}

// Gestisce le operazioni di I/O sulla lista dei topic
static long pubsub_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct pubsub_topic_req topic_req;
    struct pubsub_topic_list truncated_topic_list;
    int ret;

    switch (cmd) {
        case PUBSUB_CREATE_TOPIC:
            if (copy_from_user(&topic_req, (struct pubsub_topic_req __user *)arg, sizeof(topic_req)))
                return -EFAULT;
            ret = pubsub_create_topic(&topic_req);
            break;
        case PUBSUB_DESTROY_TOPIC:
            if (copy_from_user(&topic_req, (struct pubsub_topic_req __user *)arg, sizeof(topic_req)))
                return -EFAULT;
            ret = pubsub_destroy_topic(&topic_req);
            break;
        case PUBSUB_LIST_TOPICS:
            ret = pubsub_list_topics(&truncated_topic_list);
            if (copy_to_user((struct pubsub_topic_list __user *)arg, &truncated_topic_list, sizeof(truncated_topic_list)))
                return -EFAULT;
            break;
        default:
            return -ENOTTY;
    }

    return ret;
}

// Definisce le operazioni di file per il device
static struct file_operations pubsub_fops = {
    .owner = THIS_MODULE,
    .open = pubsub_open,
    .release = pubsub_release,
    .unlocked_ioctl = pubsub_ioctl,
};

// Inizializza il modulo
static int __init pubsub_init(void) 
{
    struct device *dev;
    int ret;

    // Alloca dinamicamente major e minor number
    ret = alloc_chrdev_region(&pubsub_base_devt, 0, PUBSUB_MAX_TOPICS + 1, "pubsub");
    if (ret < 0) {
        pr_err("alloc_chrdev_region fallito: %d\n", ret);
        return ret;
    }
    dev_num = MKDEV(MAJOR(pubsub_base_devt), 0);

    // Inizializza la struttura cdev e collega le operazioni
    cdev_init(&pubsub_cdev, &pubsub_fops);

    // Registra il cdev al sistema
    ret = cdev_add(&pubsub_cdev, dev_num, 1);
    if (ret < 0) {
        pr_err("cdev_add fallito: %d\n", ret);
        goto err1;
    }

    // Crea la classe del cdev
    pubsub_class = class_create("pubsub");
    if (IS_ERR(pubsub_class)) {
        ret = PTR_ERR(pubsub_class);
        pr_err("class_create fallito: %d\n", ret);
        goto err2;
    }

    // Aggiunge il nodo del cdev a /dev/
    dev = device_create(pubsub_class, NULL, dev_num, NULL, "pubsub_ctrl");
    if (IS_ERR(dev)) {
        ret = PTR_ERR(dev);
        pr_err("device_create fallito: %d\n", ret);
        goto err3;
    }

    pr_info("pubsub montato correttamente\n");
    return 0;

err3:
    class_destroy(pubsub_class);
err2:
    cdev_del(&pubsub_cdev);
err1:
    unregister_chrdev_region(pubsub_base_devt, PUBSUB_MAX_TOPICS + 1);
    return ret;

}

// Distrugge il modulo
static void __exit pubsub_exit(void)
{
    struct topic *t, *tmp;
    
    mutex_lock(&topic_list_mutex);

    // Distrugge i topic presenti nella lista
    list_for_each_entry_safe(t, tmp, &topic_list, list) {
        list_del(&t->list);
        pubsub_topic_device_destroy(t);
        topic_wake_all_subscribers(t);
        kref_put(&t->refcount, topic_free_kref);
    }
    
    mutex_unlock(&topic_list_mutex);
    
    // Rimuove il nodo del cdev da /dev/
    device_destroy(pubsub_class, dev_num);

    // Distrugge la classe del cdev
    class_destroy(pubsub_class);

    // Rimuove il cdev dal sistema
    cdev_del(&pubsub_cdev);

    // Rilascia major e minor number
    unregister_chrdev_region(pubsub_base_devt, PUBSUB_MAX_TOPICS + 1);

    // Distrugge l'allocatore dei minor number dei topic
    ida_destroy(&pubsub_minor_ida);

    
    pr_info("pubsub rimosso correttamente\n");
}

module_init(pubsub_init);
module_exit(pubsub_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Luca Maietti");
MODULE_DESCRIPTION("Implementazione di protocollo pub/sub concorrente per fini didattici universitari");