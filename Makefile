ROOT_DIR= $(shell pwd)
TARGETS= toolkits/bc toolkits/bfs toolkits/cc toolkits/pagerank toolkits/sssp
 MACROS=
MACROS= -D PRINT_DEBUG_MESSAGES
MACROS += -D ENABLE_BITMAP_CACHE=0
MACROS += -D ENABLE_INDEX_CACHE=0
MACROS += -D ENABLE_EDGE_CACHE=0

MPICXX= mpicxx
NUMACTL_INC=/spack/apps/linux-centos7-x86_64/gcc-8.3.0/numactl-2.0.12-d565btygk3s5kc7stvldnc6pvglwpplh/include/
NUMACTL_LIB=/spack/apps/linux-centos7-x86_64/gcc-8.3.0/numactl-2.0.12-d565btygk3s5kc7stvldnc6pvglwpplh/lib/
LIB64_LINKER=-Wl,-rpath -Wl,/spack/apps/gcc/8.3.0/lib64
CXXFLAGS= -O3 -Wall -std=c++11 -g -fopenmp -march=native -I$(ROOT_DIR) -I$(ROOT_DIR)/../ -I$(NUMACTL_INC) -L$(NUMACTL_LIB) $(LIB64_LINKER) $(MACROS)
# CXXFLAGS= -O3 -Wall -std=c++11 -g -fopenmp -march=native -I$(ROOT_DIR) -I$(ROOT_DIR)/../ $(MACROS)
SYSLIBS= -lnuma
HEADERS= $(shell find . -name '*.hpp')

all: $(TARGETS)

toolkits/%: toolkits/%.cpp $(HEADERS)
	$(MPICXX) $(CXXFLAGS) -o $@ $< $(SYSLIBS)

clean: 
	rm -f $(TARGETS)

