g++ -O2 -std=c++17 -march=armv8-a -DUSE_PMCCNTR arm_load_prfm_latency.cpp -o bench
./bench --cpu 0 --miss-mb 512

Load results after overhead subtraction:
load hit                         :      3.000 cycles/op
load miss                        :    353.379 cycles/op

PRFM results after address-generation overhead subtraction:
hint         | hit target                   | miss target                 
-------------+------------------------------+------------------------------
PLDL1KEEP    |      0.028 cycles/op |      5.478 cycles/op
PLDL1STRM    |      0.038 cycles/op |      4.882 cycles/op
PLDL2KEEP    |      0.037 cycles/op |     19.622 cycles/op
PLDL2STRM    |      0.031 cycles/op |     19.668 cycles/op
PLDL3KEEP    |      0.029 cycles/op |     23.214 cycles/op
PLDL3STRM    |      0.036 cycles/op |     21.433 cycles/op
PSTL1KEEP    |      0.007 cycles/op |      5.584 cycles/op
PSTL1STRM    |      0.034 cycles/op |      7.446 cycles/op
PSTL2KEEP    |      0.034 cycles/op |     23.947 cycles/op
PSTL2STRM    |      0.185 cycles/op |     19.222 cycles/op
PSTL3KEEP    |      0.053 cycles/op |     21.895 cycles/op
PSTL3STRM    |      0.107 cycles/op |     21.978 cycles/op
