#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>

static int pubsub_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int pubsub_release(struct inode *inode, struct file *file)
{
    return 0;
}

static long pubsub_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    return 0;
}

static dev_t dev_num;
static struct cdev pubsub_cdev;
static struct class *pubsub_class;

struct file_operations pubsub_fops = {
    .owner = THIS_MODULE,
    .open = pubsub_open,
    .release = pubsub_release,
    .unlocked_ioctl = pubsub_ioctl,
};

static int __init pubsub_init(void) 
{
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
    if (IS_ERR(device_create(pubsub_class, NULL, dev_num, NULL, "pubsub"))) {
        ret = PTR_ERR(pubsub_class);
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