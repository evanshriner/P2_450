#include "param.h"

// #define NQUEUE = 6

// the constant time quantum increase for each queue
// since timer intterupt happens every 10ms, divide
// this value by 10.
// #define TQ = 1;


struct queue {

    // array of processes
    struct proc *q[NPROC];

    // amount of processes
    int size;

    int front; // front of queue

    int back; // back of queue

    // time quantum of selected queue
    int quantum; // this is assigned during initalization of proc.c / scheduler


};

struct MLFQ {

    // static queue structure
    struct queue queues[NQUEUE];

};

