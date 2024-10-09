#ifndef MYQUEUE_H
#define MYQUEUE_H

#include <stdlib.h>

struct myqueue_node {
    void *data;
    struct myqueue_node *next;
};

struct myqueue {
    struct myqueue_node *front;
    struct myqueue_node *rear;
    int size;
};

static inline void myqueue_init(struct myqueue *q) {
    q->front = NULL;
    q->rear = NULL;
    q->size = 0;
}

static inline int myqueue_is_empty(struct myqueue *q) { return (q->size == 0); }

static inline void myqueue_add(struct myqueue *q, void *data) {
    struct myqueue_node *new_node = (struct myqueue_node *)malloc(sizeof(struct myqueue_node));
    new_node->data = data;
    new_node->next = NULL;

    if (q->rear) {
        q->rear->next = new_node;
    } else {
        q->front = new_node;
    }
    q->rear = new_node;
    q->size++;
}

static inline void *myqueue_poll(struct myqueue *q) {
    if (myqueue_is_empty(q)) {
        return NULL;
    }

    struct myqueue_node *front_node = q->front;
    void *data = front_node->data;

    q->front = front_node->next;
    if (q->front == NULL) {
        q->rear = NULL;
    }

    free(front_node);
    q->size--;

    return data;
}

static inline void *myqueue_peek(struct myqueue *q) {
    if (myqueue_is_empty(q)) {
        return NULL;
    }
    return q->front->data;
}

static inline void myqueue_free(struct myqueue *q, void (*free_func)(void *)) {
    while (!myqueue_is_empty(q)) {
        void *data = myqueue_poll(q);
        if (free_func) free_func(data);
    }
}

#endif