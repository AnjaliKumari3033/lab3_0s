#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

struct Process {
    int id = 0;
    int arrival = 0;
    std::vector<int> cpu;
    std::vector<int> io;
    int burst = 0;
    int remaining = 0;
    int level = 0;
    int quantum_used = 0;
    int finish = -1;
};

struct Running {
    int process = -1;
    int start = -1;
};

struct Segment {
    int cpu;
    int process;
    int burst;
    int start;
    int end;
};

enum class Algorithm { Fifo, RoundRobin, Mlfq };

static std::string lower(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

static Algorithm parse_algorithm(const std::string& value) {
    const std::string name = lower(value);
    if (name == "fifo" || name == "fcfs") return Algorithm::Fifo;
    if (name == "rr" || name == "round-robin" || name == "roundrobin") return Algorithm::RoundRobin;
    if (name == "mlfq") return Algorithm::Mlfq;
    throw std::invalid_argument("algorithm must be fifo, rr, or mlfq");
}

static std::vector<Process> read_workload(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open workload: " + path);

    std::vector<Process> processes;
    std::string line;
    int id = 1;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        std::istringstream row(line);
        std::vector<int> values;
        int value;
        while (row >> value) values.push_back(value);
        const std::size_t first_character = line.find_first_not_of(" \t\r");
        if (first_character == std::string::npos) continue;
        if (values.empty() || (!std::isdigit(static_cast<unsigned char>(line[first_character])) && line[first_character] != '-')) {
            throw std::runtime_error("invalid workload row at line " + std::to_string(line_number));
        }
        if (values.back() != -1 || values.size() < 2) {
            throw std::runtime_error("workload line " + std::to_string(line_number) + " must end with -1");
        }

        Process process;
        process.id = id++;
        process.arrival = values[0];
        for (std::size_t index = 1; index + 1 < values.size(); index += 2) {
            process.cpu.push_back(values[index]);
            if (index + 2 < values.size()) process.io.push_back(values[index + 1]);
        }
        if (process.cpu.empty()) throw std::runtime_error("process has no CPU burst");
        for (int duration : process.cpu) {
            if (duration <= 0) throw std::runtime_error("CPU burst durations must be positive");
        }
        for (int duration : process.io) {
            if (duration < 0) throw std::runtime_error("I/O burst durations cannot be negative");
        }
        process.remaining = process.cpu[0];
        processes.push_back(process);
    }
    return processes;
}

class Simulator {
public:
    Simulator(std::vector<Process> processes, Algorithm algorithm, int cpus, int quantum, int boost)
        : processes_(std::move(processes)), algorithm_(algorithm), cpus_(cpus), quantum_(quantum), boost_(boost),
          running_(cpus) {}

    void run() {
        int completed = 0;
        int time = 0;
        std::vector<std::vector<int>> waiting;
        waiting.resize(processes_.size());

        while (completed < static_cast<int>(processes_.size())) {
            if (algorithm_ == Algorithm::Mlfq && boost_ > 0 && time > 0 && time % boost_ == 0) {
                for (Process& process : processes_) {
                    if (process.finish < 0) process.level = 0;
                }
                for (int process : ready_) {
                    processes_[process].level = 0;
                    mlfq_[0].push(process);
                }
                ready_.clear();
                std::vector<int> boosted;
                for (auto& queue : mlfq_) {
                    while (!queue.empty()) {
                        boosted.push_back(queue.front());
                        queue.pop();
                    }
                }
                for (int process : boosted) mlfq_[0].push(process);
            }

            for (std::size_t index = 0; index < processes_.size(); ++index) {
                Process& process = processes_[index];
                if (process.finish >= 0) continue;
                if (process.arrival == time) enqueue(static_cast<int>(index));
            }
            for (std::size_t index = 0; index < waiting.size(); ++index) {
                if (!waiting[index].empty() && waiting[index][0] == time) {
                    waiting[index].clear();
                    Process& process = processes_[index];
                    ++process.burst;
                    process.remaining = process.cpu[process.burst];
                    process.quantum_used = 0;
                    enqueue(static_cast<int>(index));
                }
            }

            dispatch(time);
            for (int cpu = 0; cpu < cpus_; ++cpu) {
                if (running_[cpu].process < 0) continue;
                Process& process = processes_[running_[cpu].process];
                --process.remaining;
                ++process.quantum_used;
                ++busy_time_;
            }
            ++time;

            for (int cpu = 0; cpu < cpus_; ++cpu) {
                const int index = running_[cpu].process;
                if (index < 0) continue;
                Process& process = processes_[index];
                bool finished_burst = process.remaining == 0;
                bool expired = is_preemptive() && process.quantum_used == effective_quantum();
                if (finished_burst) {
                    close_segment(cpu, time);
                    process.quantum_used = 0;
                    if (process.burst + 1 == static_cast<int>(process.cpu.size())) {
                        process.finish = time;
                        ++completed;
                    } else {
                        waiting[index] = {time + process.io[process.burst]};
                    }
                    running_[cpu] = {};
                } else if (expired) {
                    close_segment(cpu, time);
                    if (algorithm_ == Algorithm::Mlfq && process.level < 2) ++process.level;
                    process.quantum_used = 0;
                    enqueue(index);
                    running_[cpu] = {};
                }
            }
        }
        for (int cpu = 0; cpu < cpus_; ++cpu) close_segment(cpu, time);
        print_results(time);
    }

private:
    bool is_preemptive() const { return algorithm_ != Algorithm::Fifo; }

    int effective_quantum() const {
        if (algorithm_ == Algorithm::RoundRobin) return quantum_;
        return 2;
    }

    void enqueue(int index) {
        if (algorithm_ == Algorithm::Mlfq) {
            mlfq_[processes_[index].level].push(index);
        } else {
            ready_.push_back(index);
        }
    }

    int next_ready() {
        if (algorithm_ != Algorithm::Mlfq) {
            if (ready_.empty()) return -1;
            int index = ready_.front();
            ready_.erase(ready_.begin());
            return index;
        }
        for (auto& queue : mlfq_) {
            if (!queue.empty()) {
                int index = queue.front();
                queue.pop();
                return index;
            }
        }
        return -1;
    }

    void dispatch(int time) {
        for (int cpu = 0; cpu < cpus_; ++cpu) {
            if (running_[cpu].process >= 0) continue;
            const int index = next_ready();
            if (index < 0) continue;
            running_[cpu] = {index, time};
        }
    }

    void close_segment(int cpu, int end) {
        if (running_[cpu].process < 0) return;
        schedule_.push_back({cpu, running_[cpu].process, processes_[running_[cpu].process].burst,
                             running_[cpu].start, end - 1});
    }

    void print_results(int elapsed) const {
        std::cout << "Schedule\n";
        for (int cpu = 0; cpu < cpus_; ++cpu) {
            std::cout << "CPU" << cpu << "\n";
            for (const Segment& segment : schedule_) {
                if (segment.cpu != cpu) continue;
                std::cout << "P" << processes_[segment.process].id << "," << segment.burst + 1 << "\t"
                          << segment.start << "\t" << segment.end << "\n";
            }
        }
        double total_turnaround = 0;
        int maximum = 0;
        for (const Process& process : processes_) {
            const int turnaround = process.finish - process.arrival;
            total_turnaround += turnaround;
            maximum = std::max(maximum, turnaround);
        }
        std::cout << std::fixed << std::setprecision(2)
                  << "Average turnaround time: " << (processes_.empty() ? 0 : total_turnaround / processes_.size()) << "\n"
                  << "Maximum turnaround time: " << maximum << "\n"
                  << "Elapsed runtime (I/O included): " << elapsed << "\n"
                  << "CPU runtime (I/O excluded): " << busy_time_ << "\n";
    }

    std::vector<Process> processes_;
    Algorithm algorithm_;
    int cpus_;
    int quantum_;
    int boost_;
    std::vector<Running> running_;
    std::vector<Segment> schedule_;
    std::vector<int> ready_;
    std::queue<int> mlfq_[3];
    int busy_time_ = 0;
};

int main(int argc, char** argv) {
    if (argc < 3 || argc > 6) {
        std::cerr << "Usage: " << argv[0] << " fifo <workload> [cpus]\n"
              << "       " << argv[0] << " rr <workload> [cpus] [quantum]\n"
              << "       " << argv[0] << " mlfq <workload> [cpus] [boost]\n";
        return 2;
    }
    try {
        const Algorithm algorithm = parse_algorithm(argv[1]);
        const int cpus = argc >= 4 ? std::stoi(argv[3]) : 1;
        const int quantum = algorithm == Algorithm::RoundRobin && argc >= 5 ? std::stoi(argv[4]) : 2;
        const int boost = algorithm == Algorithm::Mlfq && argc >= 5 ? std::stoi(argv[4]) : 0;
        if (cpus < 1 || cpus > 2 || quantum < 1 || boost < 0) throw std::invalid_argument("invalid numeric option");
        if (algorithm != Algorithm::Mlfq && argc == 6) throw std::invalid_argument("too many options");
        Simulator(read_workload(argv[2]), algorithm, cpus, quantum, boost).run();
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}