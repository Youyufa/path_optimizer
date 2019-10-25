# path_optimization
## QP version  
分为两阶段:  
1. 对输入的路径进行平滑，目标函数里不考虑车辆约束和障碍物，速度快；  
2. 将平滑后的路径作为参考路径进行优化，用离散的线性化的车辆模型，考虑障碍物和起点终点位姿等，在Frenet坐标系下进行。优化问题的形式是二次规划。  
