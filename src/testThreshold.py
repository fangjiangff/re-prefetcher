import subprocess
import itertools
import openpyxl
import csv

# SRC = "triggerThreshold.cc"
# SRC = "triggerThreshold-x86.cc"
# OUT = "bin/triggerThreshold-x86"
SRC = "triggerThreshold-arm.cc"
OUT = "../bin/triggerThreshold-arm"

# configs = [(1,1), (1,0), (0,1), (0,0)]
# configs = [ (0,0), (1,0)]
# // (0,0,0)miss load, (0,1,0) miss store; (1,0,0) hit load,(0,0,1) miss prefetch.
configs = [(0,0,0), (0,1,0), (1,0,0), (0,0,1)]
# micro_arch = "CascadeLake"
micro_arch = "CortexA76"
wb = openpyxl.Workbook()
# timestamp = time.strftime("%Y-%m-%d-%H-%M-%S", time.localtime())
# print(f"Start testing at {timestamp}")

for hit,st, sw in configs:
    print("="*60)
    print(f"TEST_ON_HIT={hit}, TEST_ON_ST={st}, TEST_ON_SW={sw}")
    # print(f"TEST_ON_HIT={hit}, TEST_ON_SW={sw}")

    compile_cmd = [
        "g++",
        "-std=gnu11",
        "-O0",
        "-static",
        f"-DTEST_ON_HIT={hit}",
        f"-DTEST_ON_ST={st}",
        f"-DTEST_ON_SW={sw}",
        "-o",
        OUT,
        SRC
    ]

    res = subprocess.run(compile_cmd)

    if res.returncode != 0:
        print("Compile failed")
        continue

    run = subprocess.run(
        ["taskset", "-c", "0", "./" + OUT],
        capture_output=True,
        text=True
    )

    if run.returncode != 0:
        print("Execution failed")
        continue

    output = run.stdout

    sheet_name = f"HIT{hit}-ST{st}-SW{sw}"
    ws = wb.create_sheet(sheet_name)


     # 读第一行，确定列数
    # first = output.readline().strip().split('\t')
    first = output.splitlines()[0].strip().split('\t')
    n = len(first)
    # 写表头
    ws.append(["Stride"] + [f"access{i}" for i in range(1, n + 1)])

    # 写第一行数据
    ws.append([1] + first)

    # 写后续行
    # for idx, line in enumerate(output, start=2):
    #     ws.append([idx] + line.strip().split('\t'))
    # 修复后代码
    for idx, line in enumerate(output.splitlines()[1:], start=2):
        ws.append([idx] + line.strip().split('	'))


del wb["Sheet"]
wb.save(f"res/threshold-{micro_arch}.xlsx")


print("All done.")
