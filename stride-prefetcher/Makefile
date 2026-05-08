test-sw:  test-sw.cc  utils.hh
	g++ -std=gnu11 -O0 -static -DPRFM_MODE=PLDL1KEEP -o bin/test-sw test-sw.cc 

test-single:  test-single.cc  utils.hh
	g++ -std=gnu11 -O0 -static -DPRFM_MODE=PLDL1KEEP -o bin/test-single test-single.cc 
trigger:  triggerThreshold-arm2.cc  utils.hh
	g++ -std=gnu11 -O0 -static -o bin/triggerThreshold-arm2 triggerThreshold-arm2.cc 
