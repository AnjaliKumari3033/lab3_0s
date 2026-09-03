# Process Scheduling Simulator

This implementation covers the algorithms and experiments in `3_process_scheduling.pdf`.

## Build

```sh
make
```

## Run

```sh
./scheduler fifo workload.txt [cpus]
./scheduler rr workload.txt [cpus] [quantum]
./scheduler mlfq workload.txt [cpus] [boost]
```

`cpus` is `1` or `2`. RR defaults to quantum 2. MLFQ always uses three queues with quantum 2; use boost `0` for no boost or `20` for the required periodic boost experiment. Workload rows have the form described in the PDF and end in `-1`.

The output includes the CPU schedule, average and maximum turnaround time, elapsed runtime, and total CPU runtime excluding I/O.