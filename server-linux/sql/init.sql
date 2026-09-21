-- ============================================================
-- 机房环境多指标远程监测与分析系统 —— 数据库初始化脚本
-- 任务C（Linux服务器）交付物   对应 `docs/接口规范.md` §4
-- 用法：sudo mysql < sql/init.sql
-- 注意：表结构与接口规范 §4 逐字段一致，禁止擅自增删字段
-- ============================================================

-- 1. 建库（utf8mb4）
CREATE DATABASE IF NOT EXISTS machine_room
  DEFAULT CHARACTER SET utf8mb4
  DEFAULT COLLATE utf8mb4_general_ci;

USE machine_room;

-- 2. 建表（四张）
-- 2.1 原始数据：每传感器值一行
CREATE TABLE IF NOT EXISTS sensor_data (
  id BIGINT AUTO_INCREMENT PRIMARY KEY,
  client_id VARCHAR(32) NOT NULL,
  sensor_type TINYINT NOT NULL,    -- 1温度 2湿度 3烟雾 4光照
  sensor_value DECIMAL(10,2) NOT NULL,
  collect_time DATETIME NOT NULL,
  KEY idx_time (collect_time)
);

-- 2.2 分析结果：统计周期结束生成
CREATE TABLE IF NOT EXISTS analysis_result (
  id BIGINT AUTO_INCREMENT PRIMARY KEY,
  client_id VARCHAR(32) NOT NULL,
  sensor_type TINYINT NOT NULL,
  period_start DATETIME NOT NULL, period_end DATETIME NOT NULL,
  avg_value DECIMAL(10,2), max_value DECIMAL(10,2),
  min_value DECIMAL(10,2), std_value DECIMAL(10,2),
  over_count INT DEFAULT 0,
  created_time DATETIME NOT NULL
);

-- 2.3 告警日志：超阈值时写入
CREATE TABLE IF NOT EXISTS alarm_log (
  id BIGINT AUTO_INCREMENT PRIMARY KEY,
  client_id VARCHAR(32) NOT NULL,
  sensor_type TINYINT NOT NULL,
  value DECIMAL(10,2), threshold DECIMAL(10,2),
  level VARCHAR(8),                -- LOW/MID/HIGH
  alarm_time DATETIME NOT NULL
);

-- 2.4 阈值配置：服务器启动时加载到内存
CREATE TABLE IF NOT EXISTS threshold_cfg (
  sensor_type TINYINT PRIMARY KEY,
  min_value DECIMAL(10,2), max_value DECIMAL(10,2), level VARCHAR(8)
);

-- 3. 阈值初始数据
--    温度 18~26 HIGH；湿度 40~60 MID；烟雾 上限400 HIGH；光照不告警（不插行）
INSERT INTO threshold_cfg (sensor_type, min_value, max_value, level) VALUES
  (1, 18.00,  26.00, 'HIGH'),
  (2, 40.00,  60.00, 'MID'),
  (3, NULL,  400.00, 'HIGH')
ON DUPLICATE KEY UPDATE
  min_value = VALUES(min_value),
  max_value = VALUES(max_value),
  level     = VALUES(level);

-- 4. 建独立用户并授权（程序不使用 root 运行）
--    MySQL 8.0 默认 caching_sha2_password 会让部分客户端/驱动连不上，
--    此处显式指定 mysql_native_password（见规划 S0 常见坑）。
--    用户不存在才创建，避免重复执行脚本报错。
CREATE USER IF NOT EXISTS 'iot'@'localhost'
  IDENTIFIED WITH mysql_native_password BY 'iot123456';

GRANT ALL PRIVILEGES ON machine_room.* TO 'iot'@'localhost';

FLUSH PRIVILEGES;
