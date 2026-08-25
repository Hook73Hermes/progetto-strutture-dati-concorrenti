#ifndef PUBSUB_IOCTL_H
#define PUBSUB_IOCTL_H

#include <linux/ioctl.h>

#define PUBSUB_MAX_NAME_LEN 32
#define PUBSUB_MAX_LISTED_TOPICS 16

struct pubsub_topic_req {
    char name[PUBSUB_MAX_NAME_LEN];
};

struct pubsub_topic_list {
    char names[PUBSUB_MAX_LISTED_TOPICS][PUBSUB_MAX_NAME_LEN];
    int count;
};

#define PUBSUB_IOC_MAGIC 'p'

#define PUBSUB_CREATE_TOPIC  _IOW(PUBSUB_IOC_MAGIC, 1, struct pubsub_topic_req)
#define PUBSUB_DESTROY_TOPIC _IOW(PUBSUB_IOC_MAGIC, 2, struct pubsub_topic_req)
#define PUBSUB_LIST_TOPICS   _IOR(PUBSUB_IOC_MAGIC, 3, struct pubsub_topic_list)

#endif