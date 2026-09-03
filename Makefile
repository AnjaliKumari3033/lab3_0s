CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -pedantic -O2

all: scheduler

scheduler: scheduler.cpp
	$(CXX) $(CXXFLAGS) scheduler.cpp -o scheduler

clean:
	rm -f scheduler

.PHONY: all clean