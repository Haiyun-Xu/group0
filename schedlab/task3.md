Problem 1  
a) The implementation of the SRTF queue is given below:
```
from functools import reduce

class SRTFQueue(FIFOQueue):
    """ Shortest remaining time first queue - with perfect knowledge """
    def dequeue(self, at_time):
        # find the thread with the least remaining time; in case of a tie, prefer the later enqueued thread (on the left)
        thread = reduce(lambda carry, current: carry if carry.remaining() <= current.remaining() else current, self.queue)
        self.queue.remove(thread)
        thread.wait_time += at_time - thread.last_queued
        return thread
        
def srtf(tasks, q, verbose=False):
    return scheduler(tasks, q, SRTFQueue, verbose)
```
And the output of executing workload 3 using SRTF with quantum = 2 is:
```
0: Arrival of Task 12 (ready queue length = 1)
0: Run Task 12 for duration 2 (ready queue length = 0)
1: Arrival of Task 13 (ready queue length = 1)
2: Arrival of Task 14 (ready queue length = 2)
2: IO wait for Task 12 for duration 1
2: Run Task 14 for duration 1 (ready queue length = 1)
3: Arrival of Task 15 (ready queue length = 2)
3: Wakeup of Task 12 (ready queue length = 3)
3: IO wait for Task 14 for duration 2
3: Run Task 12 for duration 2 (ready queue length = 2)
5: Wakeup of Task 14 (ready queue length = 3)
5: Run Task 14 for duration 1 (ready queue length = 2)
6: Run Task 15 for duration 2 (ready queue length = 1)
8: Run Task 15 for duration 1 (ready queue length = 1)
9: Run Task 13 for duration 2 (ready queue length = 0)
11: Run Task 13 for duration 2 (ready queue length = 0)
13: Run Task 13 for duration 2 (ready queue length = 0)
15: Run Task 13 for duration 1 (ready queue length = 0)
16: Stop
```

b) The implementation of MLFQ is given below:
```
class TwoLevelFeedbackQueue(object):
    """ Dual Queue of tasks to be scheduled """
    def __init__(self, q_int, q_cpu):
        self.queue = deque()
        self.q = q_int
        self.q_cpu = q_cpu
        self.cpu_queue = deque()
    
    def enqueue(self, thread, at_time):
        """Insert into interactive"""
        thread.last_queued = at_time
        self.queue.appendleft(thread)
        pass
        
    def enqueue_cpu(self, thread, at_time):
        """Insert into non-interactive"""
        thread.last_queued = at_time
        self.cpu_queue.appendleft(thread)
        pass
        
    def dequeue(self, at_time):
        # start with the higher-priority interactive queue, and then move onto the lower-priority background queue
        # round-robin (FIFO) on each queue, prefer the earlier enqueued in case of a tie
        # return both the thread and the quantum
        if self.queue:
            thread = self.queue.pop()
            thread.wait_time += at_time - thread.last_queued
            return thread, self.q
        elif self.cpu_queue:
            thread = self.cpu_queue.pop()
            thread.wait_time += at_time - thread.last_queued
            return thread, self.q_cpu
        else:
            return None, 0
        
    def arrive(self, thread, at_time):
        self.enqueue(thread, at_time)
    
    def wake(self, thread, at_time):
        self.enqueue(thread, at_time)
    
    def empty(self):
        return not self.queue and not self.cpu_queue
    
    def __len__(self):
        return len(self.queue) + len(self.cpu_queue)
    
    def weight(self):
        return sum([t.remaining() for t in self.queue]) + sum([t.remaining() for t in self.cpu_queue])


def mlfq2(tasks, q1, q2, verbose=False):
    remaining = TaskStream(tasks)
    ready = TwoLevelFeedbackQueue(q1, q2)
    cpu = Machine(remaining, ready, verbose)

    while not ready.empty() or cpu.next_start():
        if ready.empty():
            cpu.idle()
        else:     
            thread, quanta = ready.dequeue(cpu.time)
            run_time, status, io_time = thread.run(quanta)
            cpu.run(run_time, thread)
            
            if status == 'io' and io_time > 0:
                cpu.io_wait(thread, io_time)
            elif status == 'cpu':
                ready.enqueue_cpu(thread, cpu.time)
            elif status != 'done':                
                ready.enqueue(thread, cpu.time)      
    cpu.stop()
    return cpu
```
And the output of executing workload 3 using MLFQ with q = 2 and q_cpu = 4 is:
```
0: Arrival of Task 12 (ready queue length = 1)
0: Run Task 12 for duration 2 (ready queue length = 0)
1: Arrival of Task 13 (ready queue length = 1)
2: Arrival of Task 14 (ready queue length = 2)
2: IO wait for Task 12 for duration 1
2: Run Task 13 for duration 2 (ready queue length = 1)
3: Arrival of Task 15 (ready queue length = 2)
3: Wakeup of Task 12 (ready queue length = 3)
4: Run Task 14 for duration 1 (ready queue length = 3)
5: IO wait for Task 14 for duration 2
5: Run Task 15 for duration 2 (ready queue length = 2)
7: Wakeup of Task 14 (ready queue length = 3)
7: Run Task 12 for duration 2 (ready queue length = 3)
9: Run Task 14 for duration 1 (ready queue length = 2)
10: Run Task 13 for duration 4 (ready queue length = 1)
14: Run Task 15 for duration 1 (ready queue length = 1)
15: Run Task 13 for duration 1 (ready queue length = 0)
16: Stop
```

Problem 2:
a) The system described is a closed system, because its attributes are not affected by external factors: each CPU burst has a fixed length, and the distribution of the arrival interval (not arrival time) is entirely captured in the Xi random variable;

b) Since the service time, i.e. the CPU burst, is M, the service rate μ = 1/M;

c) For the utilization to be 50%, U = λ/μ should be 1/2, or λ = 1/(2M). This means that the mean of the arrival process Xi would be 2M - if a task arrives every 2M but the service time is M, then the utilization rate is 50%;

The implementation of the code is given below:
```
# These functions may (or may not) be useful for you to implement

def cpuUtilization(cpulog):
    # Given the Machine's log (list of tuples), computes the average CPU utilization
    busy, idle = 0, 0
    for log in cpulog:
        action = log[1]
        if action == "idle":
            idle += log[2]
        elif action == "stop":
            busy = log[0]-idle
    
    return busy/(busy+idle)

def responseTimes(cpulog):
    # Given the Machine's log, computes the response time of each task, and returns a list containing them
    # The total response time is equal to: total run time + total io wait time + total scheduling wait time
    
    # Each element of the list is a timing compilation for a task, with the following format:
    # [(last_ready_time, total_run_time, total_io_wait_time, total_scheduling_wait_time) ...];
    # where last_ready_time is the most recent timepoint at which the task became ready
    # (because of arrival or awakening);
    task_time_compilations = dict()
    for log in cpulog:
        action = log[1]
        if action == "arrive":
            task_time_compilations[log[2]] = (log[0], 0, 0, 0)
        elif action == "run":
            compilation = task_time_compilations[log[2]]
            task_time_compilations[log[2]] = (None, compilation[1]+log[3], compilation[2], compilation[3]+(log[0]-compilation[0]))
        elif action == "io wait":
            compilation = task_time_compilations[log[2]]
            task_time_compilations[log[2]] = (None, compilation[1], compilation[2]+log[3], compilation[3])
        elif action == "wakeup":
            compilation = task_time_compilations[log[2]]
            task_time_compilations[log[2]] = (log[0], compilation[1], compilation[2], compilation[3])
    
    task_response_times = []
    for _, value in task_time_compilations.items():
        # total response time = total run time + total io wait time + total scheduling wait time
        task_response_times.append(value[1]+value[2]+value[3])
    
    return task_response_times

# l and lmbdas are "recommended values" that you can change if desired
l = 10
lmbdas = np.array((0.2, 0.5, 0.7, 0.8, 0.9, 0.93, 0.95, 0.97, 0.99)) / l

# Decrease this when developing in case it takes too long
TRIALS = 100
N = 100

# The scheduler used in the experiment
test_scheduler = fcfs

response_time_medians = []
response_time_95th_percentiles = []
utilizations = []
for lmbda in lmbdas:
    trial_utilizations = []
    trial_medians = []
    trial_95ths = []
    
    # repeat each experiment for TRIAL times
    for _ in range(TRIALS):
        # create N tasks, each with a service time l, and the arrival interval
        # is exponentially distributed with parameter lmdba
        tasks = make_exp_arrivals(lmbda, l, N)
        
        # run the tasks with the test scheduler, and obtain the processor logs
        cpulog = test_scheduler(tasks).log
        
        # calculate the metrics for this trial
        response_times = responseTimes(cpulog)
        trial_medians.append(np.median(response_times))
        trial_95ths.append(np.percentile(response_times, 95))
        trial_utilizations.append(cpuUtilization(cpulog))
    
    response_time_medians.append(np.mean(trial_medians))
    response_time_95th_percentiles.append(np.mean(trial_95ths))
    utilizations.append(np.mean(trial_utilizations))
    
    # This takes a while to run so this print statement lets us track progress
    print("Finished trials at", lmbda)
```

d) As λ increases and approaches 1, the CPU utilization also increases almost linearly and approaches 1, as shown in [this plot](./Problem_2d.png);

e) As λ increases and approaches 1, the response time also increases, but not linearly, as shown in [this plot](./Problem_2e.png). The median and 95th percentile response time both increase non-linearly, and the 95th percentile response time increases slightly faster;

f) Using different schedulers didn't change the utilization growth trend (utilization plots of [round-robin](./Problem_2f_rr_utilization.png), [SRTF](./Problem_2f_srtf_utilization.png), and [MLFQ](./Problem_2f_mlfq_utilization.png)) - it still increased linearly as λ approaches 1. However, in terms of response time, the three schedulers have very different outcome:
- [round-robin](./Problem_2f_rr_latency.png) is most similar to FIFO in both growth rate and absolute magnitude, with a slightly higher median and 95th percentile response time;
- [SRTF](./Problem_2f_srtf_latency.png) is the most different from FIFO: its median response time is less than half of FIFO, but its 95th percentile response time is more than twice of FIFO. The long tail of SRTF should be attributed to how it breaks the tie between two tasks - it schedules the most-recently entered first, hence further lengthening the scheduling wait time of tasks that have already waited for a long time;
- [MLFQ](./Problem_2f_mlfq_latency.png) is also similiar to FIFO in its shape, but is quite a bit higher in terms of magnitude: both the median and 95th percentile response time are about 50% higher. This is likely due to that fact that the recently arrived tasks are always prioritized first, and so the scheduling wait time for background tasks are longer;

g) With the system working at 100% throughput (long term average), if there is bursty arrival during any time window, then the system will be overload during that time window, and the tasks in that time window will experience higher latency waiting in the ready queue - no schedule can alleviate this waiting;

Problem 3  
a) I actually can't explain why the length of the FCFS queue never exceeds 2. Feels like I'm not fully understanding the information given in the problem;

b) Since Si and Ti have the same distribution, with both S1 and T1 unknown, the probability of S1 being less than T1 is Pr[Si < Ti] = Pr[Si < Si]. There is an expression for this, but without knowing the exact distribution of Si, we cannot calculate the exact value;

c) The CPUTime(Si) = Σ_i=1-m_(Si), and so the E[CPUTime(Si)] = E[Σ_i=1-m_(Si)] = Σ_i=1-m_(E[Si]) = m*E[Si]. Then, by the CLT, the variance of CPUTime(Si) = Var(Si) / m;

I wasn't able to answer any questions after d), because either I've forgotten the probability concepts, or I couldn't implement the code asked of me. This is the end of the scheduling lab.