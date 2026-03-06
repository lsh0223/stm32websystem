/*
 Navicat Premium Data Transfer

 Source Server         : 网吧数据库
 Source Server Type    : MySQL
 Source Server Version : 80044
 Source Host           : localhost:3306
 Source Schema         : netbar

 Target Server Type    : MySQL
 Target Server Version : 80044
 File Encoding         : 65001

 Date: 15/02/2026 11:45:10
*/

SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

DROP TABLE IF EXISTS `admins`;
CREATE TABLE `admins`  (
  `id` int NOT NULL AUTO_INCREMENT,
  `username` varchar(50) NOT NULL,
  `password_hash` varchar(255) NOT NULL,
  `last_login` datetime NULL DEFAULT NULL,
  PRIMARY KEY (`id`),
  UNIQUE INDEX `username`(`username`)
) ENGINE = InnoDB DEFAULT CHARSET=utf8mb4;

INSERT INTO `admins` (`username`, `password_hash`) VALUES ('admin', 'scrypt:32768:8:1$KqW1mG4e$9c0f0a5b8109d7df9d2c20a9a13b6329e4695b2c7');

DROP TABLE IF EXISTS `users`;
CREATE TABLE `users`  (
  `id` int NOT NULL AUTO_INCREMENT,
  `username` varchar(64) NOT NULL,
  `password_hash` varchar(255) NULL, 
  `card_uid` varchar(16) NULL, 
  `id_card` varchar(20) NULL DEFAULT '',
  `birthdate` date NOT NULL DEFAULT '2000-01-01',
  `balance` decimal(10, 2) NOT NULL DEFAULT 0.00,
  `total_recharge` decimal(10,2) NOT NULL DEFAULT 0.00,
  `is_active` tinyint NOT NULL DEFAULT 1,
  `created_at` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  UNIQUE INDEX `card_uid`(`card_uid`),
  UNIQUE INDEX `username`(`username`)
) ENGINE = InnoDB DEFAULT CHARSET=utf8mb4;

DROP TABLE IF EXISTS `binding_codes`;
CREATE TABLE `binding_codes` (
  `id` int AUTO_INCREMENT PRIMARY KEY,
  `code` varchar(10) NOT NULL,
  `card_uid` varchar(20) NOT NULL,
  `id_card` varchar(20) NOT NULL,
  `created_at` datetime DEFAULT CURRENT_TIMESTAMP
) ENGINE = InnoDB DEFAULT CHARSET=utf8mb4;

DROP TABLE IF EXISTS `alarm_log`;
CREATE TABLE `alarm_log`  (
  `id` bigint NOT NULL AUTO_INCREMENT,
  `device_id` varchar(32) NOT NULL,
  `alarm_type` varchar(32) NOT NULL,
  `level` tinyint NOT NULL DEFAULT 0,
  `message` varchar(255) NULL,
  `created_at` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `is_resolved` tinyint NULL DEFAULT 0,
  PRIMARY KEY (`id`)
) ENGINE = InnoDB DEFAULT CHARSET=utf8mb4;

DROP TABLE IF EXISTS `broadcast_log`;
CREATE TABLE `broadcast_log`  (
  `id` bigint NOT NULL AUTO_INCREMENT,
  `scope` varchar(16) NOT NULL,
  `device_id` varchar(32) NULL,
  `text` varchar(255) NOT NULL,
  `created_at` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`)
) ENGINE = InnoDB DEFAULT CHARSET=utf8mb4;

DROP TABLE IF EXISTS `config`;
CREATE TABLE `config`  (
  `k` varchar(50) NOT NULL,
  `v` varchar(255) NULL,
  PRIMARY KEY (`k`)
) ENGINE = InnoDB DEFAULT CHARSET=utf8mb4;
INSERT INTO `config` (`k`, `v`) VALUES ('price_per_min', '1.0');

DROP TABLE IF EXISTS `consume_log`;
CREATE TABLE `consume_log`  (
  `id` bigint NOT NULL AUTO_INCREMENT,
  `user_id` int NOT NULL,
  `session_id` bigint NOT NULL,
  `amount` decimal(10, 2) NOT NULL,
  `created_at` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  CONSTRAINT `fk_consume_user` FOREIGN KEY (`user_id`) REFERENCES `users` (`id`)
) ENGINE = InnoDB DEFAULT CHARSET=utf8mb4;

DROP TABLE IF EXISTS `device_state_log`;
CREATE TABLE `device_state_log`  (
  `id` bigint NOT NULL AUTO_INCREMENT,
  `device_id` varchar(32) NOT NULL,
  `state_text` text NULL,
  `created_at` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`)
) ENGINE = InnoDB DEFAULT CHARSET=utf8mb4;

DROP TABLE IF EXISTS `devices`;
CREATE TABLE `devices`  (
  `id` int NOT NULL AUTO_INCREMENT,
  `device_id` varchar(32) NOT NULL,
  `seat_name` varchar(32) NOT NULL,
  `current_user_id` int NULL,
  `current_session` bigint NULL,
  `current_status` tinyint NOT NULL DEFAULT 0,
  `is_maintenance` tinyint(1) NOT NULL DEFAULT 0,
  `pc_status` tinyint NOT NULL DEFAULT 0,
  `light_status` tinyint NOT NULL DEFAULT 0,
  `human_status` tinyint NOT NULL DEFAULT 0,
  `smoke_percent` int NOT NULL DEFAULT 0,
  `current_sec` int NOT NULL DEFAULT 0,
  `current_fee` int NOT NULL DEFAULT 0,
  `last_update` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  `current_username` varchar(64) NULL,
  PRIMARY KEY (`id`),
  UNIQUE INDEX `device_id`(`device_id`)
) ENGINE = InnoDB DEFAULT CHARSET=utf8mb4;

DROP TABLE IF EXISTS `recharge_log`;
CREATE TABLE `recharge_log`  (
  `id` bigint NOT NULL AUTO_INCREMENT,
  `user_id` int NOT NULL,
  `amount` decimal(10, 2) NOT NULL,
  `balance_after` decimal(10, 2) NOT NULL,
  `operator` varchar(64) NULL,
  `remark` varchar(255) NULL,
  `created_at` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  CONSTRAINT `fk_recharge_user` FOREIGN KEY (`user_id`) REFERENCES `users` (`id`)
) ENGINE = InnoDB DEFAULT CHARSET=utf8mb4;

DROP TABLE IF EXISTS `user_session_log`;
CREATE TABLE `user_session_log`  (
  `id` bigint NOT NULL AUTO_INCREMENT,
  `user_name` varchar(64) NOT NULL,
  `device_id` varchar(32) NOT NULL,
  `card_uid` varchar(32) NULL,
  `start_time` datetime NOT NULL,
  `end_time` datetime NULL,
  `duration_sec` int NOT NULL DEFAULT 0,
  `fee` decimal(10, 2) NOT NULL,
  `end_reason` varchar(32) NULL,
  PRIMARY KEY (`id`)
) ENGINE = InnoDB DEFAULT CHARSET=utf8mb4;

SET FOREIGN_KEY_CHECKS = 1;

