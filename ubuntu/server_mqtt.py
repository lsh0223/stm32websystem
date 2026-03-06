#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import logging
import datetime
import time
import random
import threading
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
TOPIC_DOOR  = "netbar/+/door_card"
TOPIC_ALERT = "netbar/+/alert"
TOPIC_CMD   = "netbar/+/cmd" 

DB_HOST = "127.0.0.1"
DB_PORT = 3306
DB_USER = "root"
DB_PASS = "123456"
DB_NAME = "netbar"

MIN_BALANCE    = 1.0
SMOKE_ALARM_TH = 60

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")

mqtt_client = mqtt.Client(client_id=MQTT_CLIENT_ID, clean_session=True)

def get_db_connection():
    return pymysql.connect(
        host=DB_HOST, port=DB_PORT, user=DB_USER, password=DB_PASS, database=DB_NAME,
        charset="utf8mb4", autocommit=True, cursorclass=pymysql.cursors.DictCursor
    )

def get_current_price() -> float:
    conn = get_db_connection()
    price = 1.0
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT v FROM config WHERE k='price_per_min'")
            row = cur.fetchone()
            if row: price = float(row['v'])
    except Exception as e: logging.error(f"Error getting price: {e}")
    finally: conn.close()
    return price

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
        birth = identity_num[6:14] if len(identity_num) == 18 else "19" + identity_num[6:12]
        return datetime.date.today().year - int(birth[0:4])
    except: return None

def send_mqtt(device_id: str, subtopic: str, payload_str: str):
    topic = f"netbar/{device_id}/cmd" if subtopic in ("cmd", "card/resp") else f"netbar/{device_id}/{subtopic}"
    logging.info("MQTT publish: %s => %s", topic, payload_str)
    try: payload_bytes = payload_str.encode("gbk", errors="ignore")
    except: payload_bytes = payload_str.encode("utf-8", errors="ignore")
    mqtt_client.publish(topic, payload_bytes, qos=0)

def log_alarm(device_id: str, alarm_type: str, message: str):
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("INSERT INTO alarm_log (device_id, alarm_type, message, created_at) VALUES (%s, %s, %s, NOW())", (device_id, alarm_type, message))
    finally: conn.close()

def get_active_session(device_id: str) -> Optional[Dict]:
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM user_session_log WHERE device_id=%s AND end_time IS NULL ORDER BY id DESC LIMIT 1", (device_id,))
            return cur.fetchone()
    finally: conn.close()

def create_session(device_id: str, card_uid: str, user_name: str, rate: float):
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("INSERT INTO user_session_log (user_name, device_id, card_uid, start_time, end_time, duration_sec, fee) VALUES (%s, %s, %s, NOW(), NULL, 0, 0.00)", (user_name, device_id, card_uid))
    finally: conn.close()

def close_session_if_exists(device_id: str, reason: str = "normal"):
    session = get_active_session(device_id)
    
    base_price = get_current_price()
    send_mqtt(device_id, "cmd", f"set_rate;val={base_price:.2f}")

    if not session: return
    now = datetime.datetime.now()
    
    delta = now - session["start_time"]
    duration_sec = int(delta.total_seconds())
    if duration_sec < 0: duration_sec = 0
    
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM users WHERE card_uid=%s", (session["card_uid"],))
            u = cur.fetchone()
            
            discount = 1.0
            fee = 0.0
            if u:
                total_rech = float(u['total_recharge'])
                if total_rech >= 1000: discount = 0.9
                elif total_rech >= 500: discount = 0.93
                elif total_rech >= 300: discount = 0.95
                elif total_rech >= 100: discount = 0.98
            
                fee = round(duration_sec / 60.0 * (base_price * discount), 2)
                
                # ★★★ 修复安全漏洞：封顶扣费，防止超扣 ★★★
                current_bal = float(u["balance"])
                if fee > current_bal:
                    fee = current_bal # 最多扣光现有余额
            
            cur.execute("UPDATE user_session_log SET end_time=%s, duration_sec=%s, fee=%s, end_reason=%s WHERE id=%s", (now, duration_sec, fee, reason, session["id"]))
            if u and fee > 0:
                new_bal = max(0.0, current_bal - fee)
                cur.execute("UPDATE users SET balance=%s WHERE id=%s", (new_bal, u["id"]))
                cur.execute("INSERT INTO consume_log (user_id, session_id, amount, created_at) VALUES (%s, %s, %s, NOW())", (u["id"], session["id"], fee))
            cur.execute("UPDATE devices SET current_status=0, current_user_id=NULL, last_update=NOW() WHERE device_id=%s", (device_id,))
    finally: conn.close()

def save_state_to_db(device_id: str, fields: Dict[str, str], raw_payload: str):
    s = int(fields.get("s", "0") or 0)
    iu = int(fields.get("iu", "0") or 0)
    sm = int(fields.get("sm", "0") or 0)
    sec = int(fields.get("sec", "0") or 0)
    al = int(fields.get("al", "0") or 0)

    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT current_sec, current_status FROM devices WHERE device_id=%s", (device_id,))
            row = cur.fetchone()
            prev_sec = int(row["current_sec"]) if row else 0
            prev_status = int(row["current_status"]) if row else 0
            
            status = 0
            if iu == 1: status = 1     
            if sm >= SMOKE_ALARM_TH: status = 2  
            if al == 1: status = 2      

            cur.execute("""INSERT INTO devices (device_id, seat_name, current_status, pc_status, light_status, human_status, smoke_percent, current_sec, current_fee, last_update) 
                           VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, NOW()) 
                           ON DUPLICATE KEY UPDATE current_status=VALUES(current_status), pc_status=VALUES(pc_status), light_status=VALUES(light_status), human_status=VALUES(human_status), smoke_percent=VALUES(smoke_percent), current_sec=VALUES(current_sec), current_fee=VALUES(current_fee), last_update=NOW()""",
                        (device_id, device_id, status, int(fields.get("pc",0)), int(fields.get("lt",0)), int(fields.get("hm",0)), sm, sec, float(fields.get("fee",0))))
            
            if status == 2 and sm >= SMOKE_ALARM_TH and prev_status != 2:
                cur.execute("INSERT INTO alarm_log (device_id, alarm_type, message, created_at) VALUES (%s, %s, %s, NOW())", 
                            (device_id, "SMOKE", f"烟雾浓度过高: {sm}%"))
    finally: conn.close()

    session = get_active_session(device_id)
    if session and iu == 1:
        conn = get_db_connection()
        force_checkout = False
        try:
            with conn.cursor() as cur:
                cur.execute("SELECT balance, total_recharge FROM users WHERE card_uid=%s", (session["card_uid"],))
                u = cur.fetchone()
                if u:
                    current_balance = float(u["balance"])
                    total_rech = float(u['total_recharge'])
                    
                    discount = 1.0
                    if total_rech >= 1000: discount = 0.9
                    elif total_rech >= 500: discount = 0.93
                    elif total_rech >= 300: discount = 0.95
                    elif total_rech >= 100: discount = 0.98
                    
                    base_price = get_current_price()
                    actual_price = round(base_price * discount, 2)
                    
                    current_fee = (sec / 60.0) * actual_price
                    
                    if current_fee >= current_balance:
                        force_checkout = True
        finally: conn.close()

        if force_checkout:
            send_mqtt(device_id, "cmd", "checkout")
            time.sleep(0.5)
            send_mqtt(device_id, "cmd", "msg:余额耗尽，系统自动结账下机")
            close_session_if_exists(device_id, reason="balance_empty")


# 座位刷卡逻辑
def handle_card_swipe(device_id: str, payload: str):
    kv = parse_kv_payload(payload)
    card_uid = (kv.get("uid") or "").strip().upper()
    id_card = (kv.get("id") or "").strip()
    if not card_uid: return

    active = get_active_session(device_id)
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT current_status, is_maintenance FROM devices WHERE device_id=%s", (device_id,))
            dev = cur.fetchone()
            if dev and dev.get("is_maintenance"):
                send_mqtt(device_id, "cmd", "card_err;code=maint;msg=维护中禁止上机")
                return

            if active:
                if active.get("card_uid") == card_uid:
                    send_mqtt(device_id, "cmd", f"card_ok;uid={card_uid};name={active['user_name']};balance=0;sec=0")
                    return
                else:
                    send_mqtt(device_id, "cmd", "card_err;code=busy;msg=设备繁忙")
                    return

            cur.execute("SELECT * FROM users WHERE card_uid=%s", (card_uid,))
            user = cur.fetchone()
            if not user:
                cur.execute("DELETE FROM binding_codes WHERE card_uid=%s", (card_uid,))
                cur.execute("DELETE FROM binding_codes WHERE created_at < DATE_SUB(NOW(), INTERVAL 1 DAY)")
                
                code = str(random.randint(100000, 999999))
                cur.execute("INSERT INTO binding_codes (code, card_uid, id_card) VALUES (%s, %s, %s)", (code, card_uid, id_card))
                send_mqtt(device_id, "cmd", "card_err;code=unbound;msg=验证失败")
                time.sleep(0.5)
                send_mqtt(device_id, "cmd", f"msg:未绑定! 绑定码:{code} 请在网站绑定")
                return

            if not user["is_active"]:
                send_mqtt(device_id, "cmd", "card_err;code=disabled;msg=账户禁用")
                return

            age = calc_age_from_id(user["id_card"]) or 0
            if age < 18: 
                send_mqtt(device_id, "cmd", "card_err;code=underage;msg=未成年人禁止")
            elif float(user["balance"]) < MIN_BALANCE: 
                send_mqtt(device_id, "cmd", "card_err;code=low_bal;msg=余额不足")
            else:
                total_rech = float(user['total_recharge'])
                discount = 1.0
                level_name = "普通"
                if total_rech >= 1000:
                    discount = 0.9; level_name = "钻石"
                elif total_rech >= 500:
                    discount = 0.93; level_name = "黄金"
                elif total_rech >= 300:
                    discount = 0.95; level_name = "白银"
                elif total_rech >= 100:
                    discount = 0.98; level_name = "青铜"

                base_price = get_current_price()
                actual_price = round(base_price * discount, 2)

                send_mqtt(device_id, "cmd", f"set_rate;val={actual_price:.2f}")
                create_session(device_id, card_uid, user["username"], actual_price)
                cur.execute("UPDATE devices SET current_status=1, current_user_id=%s, last_update=NOW() WHERE device_id=%s", (user["id"], device_id))
                
                send_mqtt(device_id, "cmd", f"card_ok;uid={card_uid};name={user['username']};balance={float(user['balance']):.2f};sec=0")
                time.sleep(0.5)
                send_mqtt(device_id, "cmd", f"msg:{level_name}会员,专享费率{actual_price:.2f}元/分")
    finally: conn.close()

def door_open_task(device_id, username, level):
    send_mqtt(device_id, "cmd", "light_on")
    send_mqtt(device_id, "cmd", f"msg:门禁已开 {level}会员:{username} 欢迎光临")
    time.sleep(3)
    send_mqtt(device_id, "cmd", "light_off")

# 门禁刷卡逻辑
def handle_door_card(device_id: str, payload: str):
    kv = parse_kv_payload(payload)
    card_uid = (kv.get("uid") or "").strip().upper()
    id_card = (kv.get("id") or "").strip()
    
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM users WHERE card_uid=%s", (card_uid,))
            user = cur.fetchone()

            if not user:
                cur.execute("DELETE FROM binding_codes WHERE card_uid=%s", (card_uid,))
                code = str(random.randint(100000, 999999))
                cur.execute("INSERT INTO binding_codes (code, card_uid, id_card) VALUES (%s, %s, %s)", (code, card_uid, id_card))
                send_mqtt(device_id, "cmd", f"msg:未绑定! 绑定码:{code} 请在网站绑定")
            elif not user["is_active"]:
                send_mqtt(device_id, "cmd", "msg:账户禁用")
            else:
                total_rech = float(user['total_recharge'])
                level_name = "普通"
                if total_rech >= 1000: level_name = "钻石"
                elif total_rech >= 500: level_name = "黄金"
                elif total_rech >= 300: level_name = "白银"
                elif total_rech >= 100: level_name = "青铜"

                t = threading.Thread(target=door_open_task, args=(device_id, user['username'], level_name))
                t.start()
    finally: conn.close()

def handle_debug(device_id: str, payload: str):
    pass

def handle_cmd_from_device(device_id: str, payload: str):
    if "checkout" in payload:
        close_session_if_exists(device_id, reason="user_checkout")

def handle_alert(device_id: str, payload: str):
    log_alarm(device_id, "ALERT", payload)
    if "occupy" in payload:
        conn = get_db_connection()
        try:
            with conn.cursor() as cur:
                cur.execute("UPDATE devices SET current_status=2, last_update=NOW() WHERE device_id=%s", (device_id,))
        finally: conn.close()

def on_connect(client, userdata, flags, rc):
    logging.info("MQTT connected rc=%s", rc)
    if rc == 0: client.subscribe([(TOPIC_STATE, 0), (TOPIC_DEBUG, 0), (TOPIC_CARD, 0), (TOPIC_DOOR, 0), (TOPIC_ALERT, 0), (TOPIC_CMD, 0)])

def on_message(client, userdata, msg):
    try:
        topic = msg.topic
        payload = msg.payload.decode("utf-8", errors="ignore")
        parts = topic.split("/")
        if len(parts) == 3 and parts[0] == "netbar":
            did, kind = parts[1], parts[2]
            if kind == "state": save_state_to_db(did, parse_kv_payload(payload), payload)
            elif kind == "debug": handle_debug(did, payload)
            elif kind == "card": handle_card_swipe(did, payload)
            elif kind == "door_card": handle_door_card(did, payload)
            elif kind == "alert": handle_alert(did, payload)
            elif kind == "cmd": handle_cmd_from_device(did, payload)
    except Exception as e: logging.exception("Handle error: %s", e)

def main():
    if MQTT_USER: mqtt_client.username_pw_set(MQTT_USER, MQTT_PASS)
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

if __name__ == "__main__": main()