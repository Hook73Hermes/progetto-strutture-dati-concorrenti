#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/list.h>
#include "pubsub_ioctl.h"

// Lista dei topic attivi protetta da mutex
struct topic {
    char name[PUBSUB_MAX_NAME_LEN];
    struct list_head list;
};

static LIST_HEAD(topic_list);
static DEFINE_MUTEX(topic_list_mutex);

static int pubsub_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int pubsub_release(struct inode *inode, struct file *file)
{
    return 0;
}

static int pubsub_create_topic(struct pubsub_topic_req *topic_req)
{
    struct topic *t;

    // Verifica che il nome sia non nullo
    if (!topic_req || !topic_req->name[0]) {
        return -EINVAL;
    }

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

    // Inserisce il topic nella lista
    list_add_tail(&t->list, &topic_list);
    
    mutex_unlock(&topic_list_mutex);

    return 0;
}

static int pubsub_destroy_topic(struct pubsub_topic_req *topic_req)
{
    struct topic *t;
    
    mutex_lock(&topic_list_mutex);

    // Cerca il topic nella lista e lo elimina
    list_for_each_entry(t, &topic_list, list) {
        if (strncmp(t->name, topic_req->name, PUBSUB_MAX_NAME_LEN) == 0) {
            list_del(&t->list);
            mutex_unlock(&topic_list_mutex);
            kfree(t);
            return 0;
        }
    }

    mutex_unlock(&topic_list_mutex);
    return -ENOENT;
}

static int pubsub_list_topics(struct pubsub_topic_list *truncated_topic_list)
{
    struct topic *t;
    int count = 0;

    mutex_lock(&topic_list_mutex);

    // Aggiunge i nomi dei topic alla lista
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

static long pubsub_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct pubsub_topic_req topic_req;
    struct pubsub_topic_list truncated_topic_list;
    int ret;

    // Gestisce le operazioni di I/O sulla lista dei topic
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

static dev_t dev_num;
static struct cdev pubsub_cdev;
static struct class *pubsub_class;

static struct file_operations pubsub_fops = {
    .owner = THIS_MODULE,
    .open = pubsub_open,
    .release = pubsub_release,
    .unlocked_ioctl = pubsub_ioctl,
};

static int __init pubsub_init(void) 
{
    struct device *dev;
    int ret;

    // Alloca dinamicamente major e minor number
    ret = alloc_chrdev_region(&dev_num, 0, 1, "pubsub");
    if (ret < 0) {
        pr_err("alloc_chrdev_region fallito: %d\n", ret);
        return ret;
    }

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
    dev = device_create(pubsub_class, NULL, dev_num, NULL, "pubsub");
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
    unregister_chrdev_region(dev_num, 1);
    return ret;

}

static void __exit pubsub_exit(void)
{
    struct topic *t, *tmp;
    
    mutex_lock(&topic_list_mutex);

    // Distrugge i topic presenti nella lista
    list_for_each_entry_safe(t, tmp, &topic_list, list) {
        list_del(&t->list);
        kfree(t);
    }
    
    mutex_unlock(&topic_list_mutex);
    
    // Rimuove il nodo del cdev da /dev/
    device_destroy(pubsub_class, dev_num);

    // Distrugge la classe del cdev
    class_destroy(pubsub_class);

    // Rimuove il cdev dal sistema
    cdev_del(&pubsub_cdev);

    // Rilascia major e minor number
    unregister_chrdev_region(dev_num, 1);
    
    pr_info("pubsub rimosso correttamente\n");
}

module_init(pubsub_init);
module_exit(pubsub_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Luca Maietti");
MODULE_DESCRIPTION("Implementazione di protocollo pub/sub concorrente per fini didattici universitari");