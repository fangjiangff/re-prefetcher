test-sw:  test-sw.cc  utils.hh
	g++ -std=gnu11 -O0 -static -DPRFM_MODE=PLDL1KEEP -o bin/test-sw test-sw.cc 
