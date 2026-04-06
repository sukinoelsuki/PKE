.globl spin_lock
spin_lock:
    # a0 寄存器保存的是锁变量的内存地址 (例如 &lock)
    li t0, 1                # 将寄存器 t0 设为 1，代表“锁定”状态

1:                          # 这是一个局部标签，用于循环
    # 原子交换指令：
    # 1. 把 t0 的值(1)写进 a0 指向的内存
    # 2. 把该内存原来的“旧值”读出来，存入 t1
    # .aq (Acquire) 后缀告诉 CPU：在这条指令完成前，后面的读写不准提前执行
    amoswap.w.aq t1, t0, (a0)

    # 检查读出来的旧值 t1
    # 如果 t1 != 0，说明锁之前就被别人拿走了（里面本来就是1）
    # 那我们就跳回标签 1，继续循环（ Spin / 自旋）
    bnez t1, 1b             # 1b 代表向后(backward)寻找标签 1

    ret                     # 如果 t1 == 0，说明我们成功把 0 换成了 1，抢锁成功！

.globl spin_unlock
spin_unlock:
    # 释放锁：把 a0 指向的内存直接清零
    # .rl (Release) 后缀告诉 CPU：在这条指令执行前，前面的写操作必须全部完成
    # x0 是常量 0 寄存器
    amoswap.w.rl x0, x0, (a0)
    
    ret