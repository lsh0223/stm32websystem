#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import logging
import datetime
from typing import Dict, Optional

import paho.mqtt.client as mqtt
import pymysql

# ========= 基本配置 =========

MQTT_BROKER    = "1.14.163.35"
MQTT_PORT      = 1883
MQTT_CLIENT_ID = "server_core"

MQTT_USER = ""
MQTT_PASS = ""

# 订阅的主题
TOPIC_STATE = "netbar/+/state"
TOPIC_DEBUG = "netbar/+/debug"
TOPIC_CARD  = "netbar/+/card"
TOPIC_ALERT = "netbar/+/alert"

# MySQL
DB_HOST = "127.0.0.1"
DB_PORT = 3306
DB_USER = "root"
DB_PASS = "123456"
DB_NAME = "netbar"

# 业务参数
PRICE_PER_HOUR = 5.0        # 每小时单价（元）
MIN_BALANCE    = 1.0        # 余额不足阈值（元）
SMOKE_ALARM_TH = 60         # 烟雾百分比告警阈值
OFFLINE_SECS   = 30         # 用于前端判断离线（只用 last_update）

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s"
)


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
    """解析 s=1;iu=1;... 形式"""
    result: Dict[str, str] = {}
    for part in payload.split(";"):
        part = part.strip()
        if not part or "=" not in part:
            continue
        k, v = part.split("=", 1)
        result[k.strip()] = v.strip()
    return result


def calc_age_from_id(identity_num: str) -> Optional[int]:
    """根据身份证号计算年龄（简单实现）"""
    if not identity_num or len(identity_num) not in (15, 18):
        return None
    try:
        if len(identity_num) == 18:
            birth = identity_num[6:14]
            year = int(birth[0:4])
            month = int(birth[4:6])
            day = int(birth[6:8])
        else:
            birth = identity_num[6:12]
            year = int("19" + birth[0:2])
            month = int(birth[2:4])
            day = int(birth[4:6])

        bdate = datetime.date(year, month, day)
        today = datetime.date.today()
        age = today.year - bdate.year - (
            (today.month, today.day) < (bdate.month, bdate.day)
        )
        return age
    except Exception:
        return None


# ========= 业务辅助函数 =========

def send_mqtt(device_id: str, subtopic: str, payload: str):
    """
    给指定终端发 MQTT 消息。

    约定：
      - 所有控制 / 刷卡结果 通通走 netbar/<device_id>/cmd
      - 其它子主题（比如以后你自己加的）才用 netbar/<device_id>/<subtopic>
    """
    # card/resp 也映射到 cmd，这样不用改 STM32 订阅逻辑
    if subtopic in ("cmd", "card/resp"):
        topic = f"netbar/{device_id}/cmd"
    else:
        topic = f"netbar/{device_id}/{subtopic}"

    logging.info("MQTT publish: %s => %s", topic, payload)
    mqtt_client.publish(topic, payload, qos=0)

def log_alarm(device_id: str, alarm_type: str, message: str):
    """写入告警日志表"""
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            sql = """
            INSERT INTO alarm_log (device_id, alarm_type, message, created_at)
            VALUES (%s, %s, %s, NOW())
            """
            cur.execute(sql, (device_id, alarm_type, message))
    finally:
        conn.close()


def get_active_session(device_id: str) -> Optional[Dict]:
    """查询该设备是否有未结束的上机会话"""
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            sql = """
            SELECT *
              FROM user_session_log
             WHERE device_id=%s AND end_time IS NULL
          ORDER BY id DESC
             LIMIT 1
            """
            cur.execute(sql, (device_id,))
            row = cur.fetchone()
            return row
    finally:
        conn.close()


def create_session(device_id: str, card_uid: str, user_name: str):
    """新建一条上机日志记录"""
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            sql = """
            INSERT INTO user_session_log
                (user_name, device_id, card_uid,
                 start_time, end_time, duration_sec, fee, end_reason)
            VALUES (%s, %s, %s, NOW(), NULL, 0, 0.00, NULL)
            """
            cur.execute(sql, (user_name, device_id, card_uid))
    finally:
        conn.close()


def close_session_if_exists(
    device_id: str,
    reason: str = "normal",
    duration_sec_hint: Optional[int] = None,
):
    """
    如果该设备存在未结束会话，则结算本次上机：
      - 时长：优先用 duration_sec_hint（比如 devices.current_sec 里保存的秒数）
              没有的话，用 start_time ~ 当前时间 计算
      - 费用：按 PRICE_PER_HOUR 换算，精确到 0.01 元
      - 扣费：扣 users.balance，写 consume_log
      - 会话：更新 user_session_log
    """
    session = get_active_session(device_id)
    if not session:
        return

    session_id = session["id"]
    card_uid   = session.get("card_uid") or ""
    user_name  = session.get("user_name") or ""
    start_time = session["start_time"]

    now = datetime.datetime.now()

    if duration_sec_hint is not None and duration_sec_hint > 0:
        duration_sec = int(duration_sec_hint)
    else:
        duration_sec = int((now - start_time).total_seconds())

    if duration_sec < 0:
        duration_sec = 0

    # 按小时单价计算费用，精确到 0.01 元
    fee = round(duration_sec / 3600.0 * PRICE_PER_HOUR, 2)

    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            # 1) 更新会话表
            sql1 = """
            UPDATE user_session_log
               SET end_time    = %s,
                   duration_sec = %s,
                   fee          = %s,
                   end_reason   = %s
             WHERE id = %s
            """
            cur.execute(sql1, (now, duration_sec, fee, reason, session_id))

            # 2) 扣用户余额（按卡号找用户）
            if card_uid and fee > 0:
                sql2 = "SELECT id, balance FROM users WHERE card_uid=%s"
                cur.execute(sql2, (card_uid,))
                u = cur.fetchone()
                if u:
                    old_balance = float(u["balance"])
                    new_balance = old_balance - fee
                    if new_balance < 0:
                        new_balance = 0.0

                    # 更新余额
                    sql3 = "UPDATE users SET balance=%s WHERE id=%s"
                    cur.execute(sql3, (new_balance, u["id"]))

                    # 写消费流水（精确到 0.01）
                    try:
                        sql4 = """
                        INSERT INTO consume_log (user_id, amount, balance_after, remark, created_at)
                        VALUES (%s, %s, %s, %s, NOW())
                        """
                        remark = f"{device_id} 下机结算，时长 {duration_sec}s，费用 {fee} 元"
                        cur.execute(sql4, (u["id"], fee, new_balance, remark))
                    except Exception:
                        # 如果你现在 consume_log 表结构不同，先忽略，不影响主流程
                        pass

            # 3) 写 LOGOUT 日志（有 system_logs 表就记一条，没有就略过）
            try:
                sql5 = """
                INSERT INTO system_logs(log_type, device_id, card_id, content, created_at)
                VALUES ('LOGOUT', %s, %s, %s, NOW())
                """
                content = f"用户[{user_name}] 下机结算, 时长 {duration_sec}s, 费用 {fee} 元"
                cur.execute(sql5, (device_id, card_uid, content))
            except Exception:
                pass

            # 4) 设备状态改为空闲
            sql6 = """
            UPDATE devices
               SET current_status = 0,
                   last_update    = NOW()
             WHERE device_id = %s
            """
            cur.execute(sql6, (device_id,))
    finally:
        conn.close()




# ========= 处理 state 上报 =========
def save_state_to_db(device_id: str, fields: Dict[str, str], raw_payload: str):
    s   = int(fields.get("s", "0") or 0)
    iu  = int(fields.get("iu", "0") or 0)
    pc  = int(fields.get("pc", "0") or 0)
    lt  = int(fields.get("lt", "0") or 0)
    hm  = int(fields.get("hm", "0") or 0)
    sm  = int(fields.get("sm", "0") or 0)
    sec = int(fields.get("sec", "0") or 0)
    fee = float(fields.get("fee", "0") or 0)

    # 0:空闲 1:使用中 2:告警
    if sm >= SMOKE_ALARM_TH:
        current_status = 2
    elif iu == 1:
        current_status = 1
    else:
        current_status = 0

    # 先取出旧的 current_sec，用来判断“这一次是否刚刚下机”
    prev_sec = 0

    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            # 读旧状态
            cur.execute("SELECT current_sec FROM devices WHERE device_id=%s", (device_id,))
            row = cur.fetchone()
            if row:
                try:
                    prev_sec = int(row.get("current_sec") or 0)
                except Exception:
                    prev_sec = 0

            seat_name = device_id

            sql = """
            INSERT INTO devices
              (device_id, seat_name, current_status,
               pc_status, light_status, human_status,
               smoke_percent, current_sec, current_fee,
               last_update)
            VALUES
              (%s, %s, %s,
               %s, %s, %s,
               %s, %s, %s, NOW())
            ON DUPLICATE KEY UPDATE
               current_status = VALUES(current_status),
               pc_status      = VALUES(pc_status),
               light_status   = VALUES(light_status),
               human_status   = VALUES(human_status),
               smoke_percent  = VALUES(smoke_percent),
               current_sec    = VALUES(current_sec),
               current_fee    = VALUES(current_fee),
               last_update    = NOW()
            """
            cur.execute(
                sql,
                (
                    device_id, seat_name, current_status,
                    pc, lt, hm,
                    sm, sec, fee,
                ),
            )

            # 若烟雾超标，顺便写一条告警日志（简单处理，不做去重）
            if sm >= SMOKE_ALARM_TH:
                try:
                    log_alarm(device_id, "SMOKE", f"烟雾浓度超标: {sm}%")
                except Exception:
                    pass

    finally:
        conn.close()

    # === 只有从 “有计时” -> “无计时” 才认为一轮上机结束，触发结算 ===
    # 这样可以避免：
    #   - 刚刷卡的时候 MCU 仍在 iu=0、sec=0 状态就被提前结算
    #   - 同一次刷卡多次上报导致多条上机记录
    if prev_sec > 0 and sec == 0:
        close_session_if_exists(device_id, reason="normal", duration_sec_hint=prev_sec)



# ========= 处理刷卡 =========

def handle_card_swipe(device_id: str, payload: str):
    """
    STM32 上报的刷卡消息：
        主题: netbar/<device_id>/card
        payload: uid=031368FC

    验证逻辑：
      1. 看该设备是否有未结束会话：
         - 如果有 + 设备状态空闲：认为是残留，会话强制结束（fee=0），然后继续本次刷卡
         - 如果有 + 设备状态使用中：
             - 同一张卡重复刷：返回 card_ok，当作“查看信息”
             - 不同卡：返回 device_busy，禁止抢座
      2. users 表中是否存在 card_uid
      3. is_active 是否为 1
      4. 身份证号计算年龄，未成年人禁止上机
      5. 余额是否 >= MIN_BALANCE
      通过后：
        - 写入 user_session_log 一条上机记录（只写 start_time）
        - 更新 devices.current_status = 1
        - 通过 cmd 主题回复 card_ok
    """
    kv = parse_kv_payload(payload)
    card_uid = kv.get("uid") or kv.get("card") or ""
    card_uid = card_uid.strip().upper()

    logging.info("handle_card_swipe: device=%s card=%s", device_id, card_uid)

    if not card_uid:
        return

    # 1) 先查有没有“未结束会话”
    active = get_active_session(device_id)

    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            # 查一下当前设备状态（0:空闲 1:上机中 2:告警）
            cur.execute(
                "SELECT current_status, current_sec FROM devices WHERE device_id=%s",
                (device_id,),
            )
            dev = cur.fetchone()
            if dev and dev.get("current_status") is not None:
                curr_status = int(dev["current_status"])
            else:
                curr_status = 0

            if active:
                active_card = (active.get("card_uid") or "").upper()

                if curr_status == 0:
                    # 有会话记录但设备显示空闲 -> 认为是“残留会话”，强制结束，不计费
                    try:
                        cur.execute(
                            """
                            UPDATE user_session_log
                               SET end_time     = start_time,
                                   duration_sec = 0,
                                   fee          = 0,
                                   end_reason   = 'stale_auto_close'
                             WHERE id = %s AND end_time IS NULL
                            """,
                            (active["id"],),
                        )
                        logging.info(
                            "auto close stale session: device=%s session_id=%s",
                            device_id,
                            active["id"],
                        )
                    except Exception:
                        pass
                    # 清理完以后，当成没有活动会话
                    active = None

                else:
                    # 设备认为“正在使用中”
                    if active_card == card_uid:
                        # 同一张卡重复刷，认为是“查看信息”，不新建会话
                        cur.execute(
                            """
                            SELECT username, id_card, balance, is_active
                              FROM users
                             WHERE card_uid=%s
                            """,
                            (card_uid,),
                        )
                        user = cur.fetchone()
                        if not user:
                            send_mqtt(device_id, "cmd", "card_err;code=invalid")
                            return

                        username = user["username"]
                        id_card  = (user.get("id_card") or "").strip()
                        balance  = float(user.get("balance", 0.0))
                        age      = calc_age_from_id(id_card) or 0

                        payload_ok = (
                            f"card_ok;uid={card_uid};name={username};"
                            f"balance={balance:.2f};age={age}"
                        )
                        send_mqtt(device_id, "cmd", payload_ok)
                        return
                    else:
                        # 另一张卡想抢当前座位
                        send_mqtt(device_id, "cmd", "card_err;code=device_busy")
                        return

            # 走到这里：
            #   - 要么根本没有活动会话
            #   - 要么刚刚清理掉了残留会话
            # 接下来正常验证本次刷卡

            # 2) 查找用户
            cur.execute(
                """
                SELECT username, id_card, balance, is_active
                  FROM users
                 WHERE card_uid=%s
                """,
                (card_uid,),
            )
            user = cur.fetchone()

            if not user:
                # 无此卡
                send_mqtt(device_id, "cmd", "card_err;code=invalid")
                return

            username  = user["username"]
            id_card   = (user.get("id_card") or "").strip()
            balance   = float(user.get("balance", 0.0))
            is_active = int(user.get("is_active", 0))

            # 3) 是否被禁用
            if not is_active:
                send_mqtt(device_id, "cmd", "card_err;code=disabled")
                return

            # 4) 年龄判断
            age = calc_age_from_id(id_card) or 0
            if age and age < 18:
                send_mqtt(device_id, "cmd", "card_err;code=underage")
                return

            # 5) 余额判断
            if balance < MIN_BALANCE:
                send_mqtt(device_id, "cmd", "card_err;code=balance_low")
                return

            # 6) 一切 ok -> 创建会话日志
            create_session(device_id, card_uid, username)

            # 更新设备当前状态为“上机中”
            cur.execute(
                """
                INSERT INTO devices
                  (device_id, seat_name, current_status, last_update)
                VALUES
                  (%s, %s, 1, NOW())
                ON DUPLICATE KEY UPDATE
                   current_status = 1,
                   last_update    = NOW()
                """,
                (device_id, device_id),
            )

            # 写 LOGIN 日志（有 system_logs 表才会成功）
            try:
                cur.execute(
                    """
                    INSERT INTO system_logs(log_type, device_id, card_id, content, created_at)
                    VALUES ('LOGIN', %s, %s, %s, NOW())
                    """,
                    (
                        device_id,
                        card_uid,
                        f"用户[{username}] 刷卡上机，初始余额 {balance:.2f} 元",
                    ),
                )
            except Exception:
                pass

    finally:
        conn.close()

    # 7) 回复 STM32：刷卡成功
    # 为了避免中文乱码，只传英文/数字，中文提示在 STM32 本地做映射
    payload_ok = (
        f"card_ok;uid={card_uid};name={username};"
        f"balance={balance:.2f};age={age}"
    )
    send_mqtt(device_id, "cmd", payload_ok)




# ========= 处理占座/其它告警 =========

def handle_alert(device_id: str, payload: str):
    alert = payload.strip()
    logging.info("handle_alert: device=%s alert=%s", device_id, alert)

    # 写告警日志
    if alert == "occupy_over_120s":
        log_alarm(device_id, "OCCUPY", "座位占用超过 120 秒")
        # 同时把该座位状态置为告警，方便主页红色显示
        conn = get_db_connection()
        try:
            with conn.cursor() as cur:
                sql = """
                UPDATE devices
                   SET current_status = 2,
                       last_update    = NOW()
                 WHERE device_id = %s
                """
                cur.execute(sql, (device_id,))
        finally:
            conn.close()

    else:
        # 其它类型也简单记录一下
        log_alarm(device_id, "OTHER", alert)


def save_debug_to_db(device_id: str, payload: str):
    """调试信息写入 device_state_log 表（如果有的话）"""
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            sql = """
            INSERT INTO device_state_log (device_id, state_text, created_at)
            VALUES (%s, %s, NOW())
            """
            cur.execute(sql, (device_id, payload))
    finally:
        conn.close()


# ========= MQTT 回调 =========

def on_connect(client: mqtt.Client, userdata, flags, rc):
    logging.info("MQTT connected rc=%s", rc)
    if rc == 0:
        client.subscribe([
            (TOPIC_STATE, 0),
            (TOPIC_DEBUG, 0),
            (TOPIC_CARD,  0),
            (TOPIC_ALERT, 0),
        ])
        logging.info("MQTT subscribed: %s, %s, %s, %s",
                     TOPIC_STATE, TOPIC_DEBUG, TOPIC_CARD, TOPIC_ALERT)
    else:
        logging.error("MQTT connect failed rc=%s", rc)


def on_message(client: mqtt.Client, userdata, msg: mqtt.MQTTMessage):
    topic = msg.topic
    payload = msg.payload.decode("utf-8", errors="ignore")

    logging.info("MQTT msg: topic=%s payload=%s", topic, payload)

    parts = topic.split("/")
    if len(parts) != 3 or parts[0] != "netbar":
        return

    device_id = parts[1]
    kind      = parts[2]

    try:
        if kind == "state":
            fields = parse_kv_payload(payload)
            save_state_to_db(device_id, fields, payload)

        elif kind == "debug":
            save_debug_to_db(device_id, payload)

        elif kind == "card":
            handle_card_swipe(device_id, payload)

        elif kind == "alert":
            handle_alert(device_id, payload)

    except Exception as e:
        logging.exception("handle message error: %s", e)


# ========= main =========

mqtt_client = mqtt.Client(client_id=MQTT_CLIENT_ID, clean_session=True)
if MQTT_USER:
    mqtt_client.username_pw_set(MQTT_USER, MQTT_PASS)

mqtt_client.on_connect = on_connect
mqtt_client.on_message = on_message


def main():
    mqtt_client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    logging.info("MQTT connecting to %s:%d ...", MQTT_BROKER, MQTT_PORT)
    mqtt_client.loop_forever()


if __name__ == "__main__":
    main()
