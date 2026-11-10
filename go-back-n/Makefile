CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra

all: sender receiver

sender: sender.cpp common.hpp
	$(CXX) $(CXXFLAGS) -o sender sender.cpp

receiver: receiver.cpp common.hpp
	$(CXX) $(CXXFLAGS) -o receiver receiver.cpp

clean:
	rm -f sender receiver
