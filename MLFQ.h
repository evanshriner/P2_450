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


    // since this is a circular queue, you are just going to mimic
    // moving the processes to the front -- really you are just going
    // to increment the front position and maintain the back position
    // until it is larger than the queue size (NPROC), then just
    // set the back to the inital position
    int front; // front of queue

    int back; // back of queue

    // time quantum of selected queue
    int quantum; // this is assigned during initalization of proc.c / scheduler


};

struct MLFQ {

    // static queue structure
    struct queue queues[NQUEUE];

};

