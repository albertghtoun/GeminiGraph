ROOT_DIR= $(shell pwd)
TARGETS= toolkits/bc toolkits/bfs toolkits/cc toolkits/pagerank toolkits/sssp
 MACROS=
# MACROS= -D PRINT_DEBUG_MESSAGES
MACROS += -D ENABLE_BITMAP_CACHE=1
MACROS += -D ENABLE_INDEX_CACHE=1
MACROS += -D ENABLE_EDGE_CACHE=1

MPICXX= mpicxx
CXXFLAGS= -O3 -Wall -std=c++11 -g -fopenmp -march=native -I$(ROOT_DIR) -I$(ROOT_DIR)/../ $(MACROS)
SYSLIBS= -lnuma
HEADERS= $(shell find . -name '*.hpp')

all: $(TARGETS)

toolkits/%: toolkits/%.cpp $(HEADERS)
	$(MPICXX) $(CXXFLAGS) -o $@ $< $(SYSLIBS)

clean: 
	rm -f $(TARGETS)

