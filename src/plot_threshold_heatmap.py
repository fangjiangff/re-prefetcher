import pandas as pd
import seaborn as sns
import matplotlib.pyplot as plt
import os

# 配置部分
INPUT_FILE = "../res/threshold-CortexA76.xlsx"
OUTPUT_DIR = "../res/heatmaps"

# 指定需要生成热力图的工作表名称列表
# 这些名称对应 testThreshold.py 生成的 sheet_name
# TARGET_SHEETS = [
#     "CascadeLake-HIT0-ST0-SW0",  # Miss Load
#     "CascadeLake-HIT0-ST1-SW0",  # Miss Store
#     "CascadeLake-HIT1-ST0-SW0",  # Hit Load
#     "CascadeLake-HIT0-ST0-SW1",  # Miss Prefetch
# ]

TARGET_SHEETS = [
    "HIT0-ST0-SW0",  # Miss Load
    "HIT0-ST1-SW0",  # Miss Store
    "HIT1-ST0-SW0",  # Hit Load
    "HIT0-ST0-SW1",  # Miss Prefetch
]

def plot_heatmaps():
    # 检查输入文件是否存在
    if not os.path.exists(INPUT_FILE):
        print(f"Error: Input file '{INPUT_FILE}' not found.")
        return

    # 创建输出目录
    if not os.path.exists(OUTPUT_DIR):
        os.makedirs(OUTPUT_DIR)
        print(f"Created output directory: {OUTPUT_DIR}")

    print(f"Reading data from {INPUT_FILE}...")
    try:
        # 加载 Excel 文件
        xls = pd.ExcelFile(INPUT_FILE)
    except Exception as e:
        print(f"Failed to open Excel file: {e}")
        return

    valid_sheets = []
    for sheet_name in TARGET_SHEETS:
        if sheet_name in xls.sheet_names:
            valid_sheets.append(sheet_name)
        else:
            print(f"Warning: Sheet '{sheet_name}' not found in workbook. Skipping.")

    num_sheets = len(valid_sheets)
    if num_sheets == 0:
        print("No valid sheets found.")
        return

    # 创建子图：1行 num_sheets 列
    fig, axes = plt.subplots(1, num_sheets, figsize=(6 * num_sheets, 8))
    if num_sheets == 1:
        axes = [axes]

    for i, sheet_name in enumerate(valid_sheets):
        print(f"Processing sheet: {sheet_name}")
        ax = axes[i]
        try:
            # 读取工作表数据
            df = pd.read_excel(xls, sheet_name=sheet_name)
            
            # 将第一列 'Stride' 设为索引
            if not df.empty:
                df.set_index(df.columns[0], inplace=True)

            # 绘制热力图
            sns.heatmap(df, cmap="viridis", annot=False, ax=ax, vmin=20, vmax=600, cbar=(i == num_sheets - 1), yticklabels=(i == 0))
            
            ax.set_title(f"{sheet_name}")
            ax.set_xlabel("Access Sequence")
            ax.set_ylabel("Stride / Index" if i == 0 else "")

        except Exception as e:
            print(f"Error processing {sheet_name}: {e}")
            ax.text(0.5, 0.5, "Error", ha='center', va='center')

    plt.tight_layout()
    output_path = os.path.join(OUTPUT_DIR, "combined_heatmaps.png")
    plt.savefig(output_path, dpi=300)
    plt.close()
    print(f"Saved combined heatmap to {output_path}")
    print("All done.")

if __name__ == "__main__":
    plot_heatmaps()