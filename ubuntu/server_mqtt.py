#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import logging
import datetime
import time
from typing import Dict, Optional
import paho.mqtt.client as mqtt
import pymysql

# ========= 基本配置 =========
MQTT_BROKER    = "127.0.0.1"
MQTT_PORT      = 1883
MQTT_CLIENT_ID = "server_core"
MQTT_USER = ""
MQTT_PASS = ""

TOPIC_STATE = "netbar/+/state"
TOPIC_DEBUG = "netbar/+/debug"
TOPIC_CARD  = "netbar/+/card"
TOPIC_ALERT = "netbar/+/alert"

DB_HOST = "127.0.0.1"
DB_PORT = 3306
DB_USER = "root"
DB_PASS = "123456"
DB_NAME = "netbar"

PRICE_PER_MIN  = 1.0
MIN_BALANCE    = 1.0
SMOKE_ALARM_TH = 60

# 简单的内存缓存，用于防止报警日志刷屏 { "device_id_type": timestamp }
alarm_cooldown = {}

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")

mqtt_client = mqtt.Client(client_id=MQTT_CLIENT_ID, clean_session=True)

# ========= DB 工具 =========

def get_db_connection():
    return pymysql.connect(
        host=DB_HOST,
        port=DB_PORT,
        user=DB_USER,
        password=DB_PASS,
        database=DB_NAME,
        charset="utf8mb4",
        autocommit=True,
        cursorclass=pymysql.cursors.DictCursor,
    )

def parse_kv_payload(payload: str) -> Dict[str, str]:
    result = {}
    for part in payload.split(";"):
        if "=" in part:
            k, v = part.split("=", 1)
            result[k.strip()] = v.strip()
    return result

def calc_age_from_id(identity_num: str) -> Optional[int]:
    if not identity_num or len(identity_num) not in (15, 18): return None
    try:
        if len(identity_num) == 18:
            birth = identity_num[6:14]
            year = int(birth[0:4])
        else:
            birth = identity_num[6:12]
            year = int("19" + birth[0:2])
        today = datetime.date.today()
        return today.year - year
    except:
        return None

# ========= 业务辅助函数 =========

def send_mqtt(device_id: str, subtopic: str, payload_str: str):
    if subtopic in ("cmd", "card/resp"):
        topic = f"netbar/{device_id}/cmd"
    else:
        topic = f"netbar/{device_id}/{subtopic}"

    logging.info("MQTT publish: %s => %s", topic, payload_str)
    
    try:
        payload_bytes = payload_str.encode("gbk", errors="ignore")
    except:
        payload_bytes = payload_str.encode("utf-8", errors="ignore")
        
    mqtt_client.publish(topic, payload_bytes, qos=0)

def log_alarm(device_id: str, alarm_type: str, message: str):
    # 防刷屏逻辑：同一台设备同一种报警，60秒内只记一次
    key = f"{device_id}_{alarm_type}"
    now = time.time()
    if key in alarm_cooldown:
        if now - alarm_cooldown[key] < 60:
            return 
    alarm_cooldown[key] = now

    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("INSERT INTO alarm_log (device_id, alarm_type, message, created_at) VALUES (%s, %s, %s, NOW())", (device_id, alarm_type, message))
            logging.info(f"ALARM LOGGED: {device_id} - {message}")
    finally:
        conn.close()

def get_active_session(device_id: str) -> Optional[Dict]:
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM user_session_log WHERE device_id=%s AND end_time IS NULL ORDER BY id DESC LIMIT 1", (device_id,))
            return cur.fetchone()
    finally:
        conn.close()

def create_session(device_id: str, card_uid: str, user_name: str):
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("INSERT INTO user_session_log (user_name, device_id, card_uid, start_time, end_time, duration_sec, fee) VALUES (%s, %s, %s, NOW(), NULL, 0, 0.00)", (user_name, device_id, card_uid))
    finally:
        conn.close()

def close_session_if_exists(device_id: str, reason: str = "normal", duration_sec_hint: Optional[int] = None):
    session = get_active_session(device_id)
    if not session: return

    now = datetime.datetime.now()
    if duration_sec_hint is not None and duration_sec_hint > 0:
        duration_sec = int(duration_sec_hint)
    else:
        duration_sec = int((now - session["start_time"]).total_seconds())

    if duration_sec < 0: duration_sec = 0
    duration_min = duration_sec / 60.0
    fee = round(duration_min * PRICE_PER_MIN, 2)

    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("UPDATE user_session_log SET end_time=%s, duration_sec=%s, fee=%s, end_reason=%s WHERE id=%s", (now, duration_sec, fee, reason, session["id"]))
            if session["card_uid"] and fee > 0:
                cur.execute("SELECT id, balance FROM users WHERE card_uid=%s", (session["card_uid"],))
                u = cur.fetchone()
                if u:
                    new_bal = max(0.0, float(u["balance"]) - fee)
                    cur.execute("UPDATE users SET balance=%s WHERE id=%s", (new_bal, u["id"]))
                    cur.execute("INSERT INTO consume_log (user_id, session_id, amount, created_at) VALUES (%s, %s, %s, NOW())", (u["id"], session["id"], fee))
            cur.execute("UPDATE devices SET current_status=0, current_user_id=NULL, last_update=NOW() WHERE device_id=%s", (device_id,))
    finally:
        conn.close()

# ========= 核心业务逻辑 =========

def save_state_to_db(device_id: str, fields: Dict[str, str], raw_payload: str):
    s   = int(fields.get("s", "0") or 0)
    iu  = int(fields.get("iu", "0") or 0)
    sm  = int(fields.get("sm", "0") or 0)
    sec = int(fields.get("sec", "0") or 0)
    
    # ★★★ 修复点1：读取 occ (Occupancy Alarm) 状态 ★★★
    # STM32 新代码会发送 occ=1 表示正在占座报警
    occ = int(fields.get("occ", "0") or 0)
    
    # ★★★ 修复点2：如果烟雾超标，记录日志 ★★★
    if sm >= SMOKE_ALARM_TH:
        log_alarm(device_id, "SMOKE", f"烟雾浓度超标: {sm}%")

    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT current_sec FROM devices WHERE device_id=%s", (device_id,))
            row = cur.fetchone()
            prev_sec = int(row["current_sec"]) if row else 0
            
            # ★★★ 修复点3：状态判定逻辑，优先显示告警(2) ★★★
            # 如果 烟雾超标 OR 占座报警(occ=1)，则状态为 2
            if sm >= SMOKE_ALARM_TH or occ == 1:
                status = 2
            elif iu == 1:
                status = 1
            else:
                status = 0
            
            cur.execute("""
                INSERT INTO devices (device_id, seat_name, current_status, pc_status, light_status, human_status, smoke_percent, current_sec, current_fee, last_update) 
                VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, NOW()) 
                ON DUPLICATE KEY UPDATE 
                    current_status=VALUES(current_status), pc_status=VALUES(pc_status), 
                    light_status=VALUES(light_status), human_status=VALUES(human_status), 
                    smoke_percent=VALUES(smoke_percent), current_sec=VALUES(current_sec), 
                    current_fee=VALUES(current_fee), last_update=NOW()
                """,
                (device_id, device_id, status, int(fields.get("pc",0)), int(fields.get("lt",0)), int(fields.get("hm",0)), sm, sec, float(fields.get("fee",0))))
    finally:
        conn.close()

    if prev_sec > 0 and sec == 0:
        close_session_if_exists(device_id, reason="normal", duration_sec_hint=prev_sec)

def handle_card_swipe(device_id: str, payload: str):
    kv = parse_kv_payload(payload)
    card_uid = (kv.get("uid") or "").strip().upper()
    if not card_uid: return

    logging.info("handle_card_swipe: device=%s card=%s", device_id, card_uid)

    active = get_active_session(device_id)
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT current_status FROM devices WHERE device_id=%s", (device_id,))
            dev = cur.fetchone()
            curr_status = int(dev["current_status"]) if dev else 0

            if active:
                if curr_status == 0:
                    logging.info("Auto-closing zombie session for %s", device_id)
                    cur.execute("UPDATE user_session_log SET end_time=NOW(), fee=0, end_reason='stale' WHERE id=%s", (active["id"],))
                    active = None 
                elif active.get("card_uid") == card_uid:
                    cur.execute("SELECT * FROM users WHERE card_uid=%s", (card_uid,))
                    u = cur.fetchone()
                    if u: send_mqtt(device_id, "cmd", f"card_ok;uid={card_uid};name={u['username']};balance={float(u['balance']):.2f};age=0")
                    return
                else:
                    send_mqtt(device_id, "cmd", "card_err;code=busy;msg=设备繁忙")
                    return

            cur.execute("SELECT * FROM users WHERE card_uid=%s", (card_uid,))
            user = cur.fetchone()
            
            if not user:
                send_mqtt(device_id, "cmd", "card_err;code=invalid;msg=无效卡")
                return
            if not user["is_active"]:
                send_mqtt(device_id, "cmd", "card_err;code=disabled;msg=账户禁用")
                return
            
            age = calc_age_from_id(user["id_card"]) or 0
            if age < 18:
                send_mqtt(device_id, "cmd", "card_err;code=underage;msg=未成年人禁止")
                return
            if float(user["balance"]) < MIN_BALANCE:
                send_mqtt(device_id, "cmd", "card_err;code=low_bal;msg=余额不足")
                return

            create_session(device_id, card_uid, user["username"])
            cur.execute("UPDATE devices SET current_status=1, current_user_id=%s, last_update=NOW() WHERE device_id=%s", (user["id"], device_id))
            send_mqtt(device_id, "cmd", f"card_ok;uid={card_uid};name={user['username']};balance={float(user['balance']):.2f};age={age}")

    finally:
        conn.close()

def handle_debug(device_id: str, payload: str):
    if "sync" in payload or "boot" in payload:
        logging.info("Device %s requesting SYNC...", device_id)
        session = get_active_session(device_id)
        if session:
            now = datetime.datetime.now()
            duration_sec = int((now - session["start_time"]).total_seconds())
            conn = get_db_connection()
            try:
                with conn.cursor() as cur:
                    cur.execute("SELECT balance, username FROM users WHERE card_uid=%s", (session["card_uid"],))
                    u = cur.fetchone()
                    if u:
                        cmd = f"restore_session;name={u['username']};balance={float(u['balance']):.2f};sec={duration_sec}"
                        send_mqtt(device_id, "cmd", cmd)
                        logging.info("Restored session for %s: %s", device_id, cmd)
            finally:
                conn.close()
    save_debug_to_db(device_id, payload)

def save_debug_to_db(device_id: str, payload: str):
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("INSERT INTO device_state_log (device_id, state_text, created_at) VALUES (%s, %s, NOW())", (device_id, payload))
    finally:
        conn.close()

def handle_alert(device_id: str, payload: str):
    log_alarm(device_id, "ALERT", payload)
    if "occupy" in payload:
        conn = get_db_connection()
        try:
            with conn.cursor() as cur:
                cur.execute("UPDATE devices SET current_status=2, last_update=NOW() WHERE device_id=%s", (device_id,))
        finally:
            conn.close()

# ========= MQTT 启动 =========

def on_connect(client, userdata, flags, rc):
    logging.info("MQTT connected rc=%s", rc)
    if rc == 0:
        client.subscribe([
            (TOPIC_STATE, 0),
            (TOPIC_DEBUG, 0),
            (TOPIC_CARD, 0),
            (TOPIC_ALERT, 0)
        ])

def on_message(client, userdata, msg):
    try:
        topic = msg.topic
        payload = msg.payload.decode("utf-8", errors="ignore")
        logging.info("MSG: %s %s", topic, payload)
        
        parts = topic.split("/")
        if len(parts) == 3 and parts[0] == "netbar":
            did, kind = parts[1], parts[2]
            if kind == "state":
                save_state_to_db(did, parse_kv_payload(payload), payload)
            elif kind == "debug":
                handle_debug(did, payload)
            elif kind == "card":
                handle_card_swipe(did, payload)
            elif kind == "alert":
                handle_alert(did, payload)
                
    except Exception as e:
        logging.exception("Handle message error: %s", e)

def main():
    if MQTT_USER:
        mqtt_client.username_pw_set(MQTT_USER, MQTT_PASS)
        
    mqtt_client.on_connect = on_connect
    mqtt_client.on_message = on_message
    
    while True:
        try:
            mqtt_client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
            logging.info("MQTT connecting to %s:%d ...", MQTT_BROKER, MQTT_PORT)
            mqtt_client.loop_forever()
        except Exception as e:
            logging.error(f"MQTT connect error: {e}")
            time.sleep(5)

if __name__ == "__main__":
    main()
    