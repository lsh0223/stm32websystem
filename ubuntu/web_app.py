#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Flask 网页后台（新版）

功能：
- 实时查看所有座位状态（空闲 / 使用中 / 告警 / 离线 / 维护中）
- 远程控制：开机 / 关机 / 开灯 / 关灯 / 下机结算 / 远程复位 / 维护中
- 向指定终端或全部终端发送短消息
- 用户账户管理：增删改查、手动充值
- 查询用户的上机记录 / 充值记录 / 扣费记录
- 基础运营报表：每日收入统计
- 日志查询：用户上机日志 / 告警日志
"""

from flask import Flask, render_template, redirect, url_for, request
import pymysql
import paho.mqtt.client as mqtt
from datetime import datetime, timedelta

# ========= MQTT 配置 =========
MQTT_BROKER = "1.14.163.35"
MQTT_PORT   = 1883
MQTT_CLIENT_ID = "web_admin"
MQTT_USER   = ""
MQTT_PASS   = ""

# ========= MySQL 配置 =========
DB_HOST = "127.0.0.1"
DB_PORT = 3306
DB_USER = "root"
DB_PASS = "123456"
DB_NAME = "netbar"

# 离线判定阈值（秒）
OFFLINE_THRESHOLD = 60

# ========= 数据库工具函数 =========

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


OFFLINE_SECS = 60   # 判定离线的阈值（秒）

def load_seats():
    """从 devices 表里读取所有座位当前状态"""
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute(
                """
                SELECT device_id, seat_name, current_status,
                       is_maintenance,
                       pc_status, light_status, human_status,
                       smoke_percent, current_sec, current_fee,
                       last_update
                  FROM devices
              ORDER BY device_id
                """
            )
            rows = cur.fetchall()
    finally:
        conn.close()

    # 这里用的是 from datetime import datetime，所以写 datetime.now()
    now = datetime.now()

    for r in rows:
        last = r["last_update"]
        is_maint = bool(r.get("is_maintenance", 0))

        # 电脑 / 灯 / 座位 文本
        r["pc_text"]    = "开机" if r["pc_status"] else "关机"
        r["light_text"] = "开启" if r["light_status"] else "关闭"
        r["human_text"] = "有人" if r["human_status"] else "无人"

        # 烟雾等级
        sm = r["smoke_percent"] or 0
        if sm < 20:
            r["smoke_level"] = "良好"
        elif sm < 40:
            r["smoke_level"] = "中等"
        elif sm < 60:
            r["smoke_level"] = "偏高"
        else:
            r["smoke_level"] = "危险"

        # 判断是否离线
        is_offline = False
        if last is None:
            is_offline = True
        else:
            # last_update 一般是 datetime 类型
            if isinstance(last, datetime):
                diff = (now - last).total_seconds()
                if diff > OFFLINE_SECS:
                    is_offline = True

        r["is_offline"]     = is_offline
        r["is_maintenance"] = is_maint

        # 组合状态文字 + 颜色
        if is_offline:
            r["status_text"]  = "离线"
            r["status_class"] = "badge bg-dark"
        elif is_maint:
            r["status_text"]  = "维护中"
            r["status_class"] = "badge bg-info"
        else:
            cs = r["current_status"] or 0  # 0 空闲 1 上机中 2 告警
            if cs == 1:
                r["status_text"]  = "上机中"
                r["status_class"] = "badge bg-success"
            elif cs == 2:
                r["status_text"]  = "告警"
                r["status_class"] = "badge bg-danger"
            else:
                r["status_text"]  = "空闲"
                r["status_class"] = "badge bg-secondary"

        # 费用格式化两位小数
        fee = r["current_fee"] or 0
        r["current_fee_fmt"] = f"{fee:.2f}"

    return rows



# ========= MQTT 客户端 =========

mqtt_client = mqtt.Client(client_id=MQTT_CLIENT_ID, clean_session=True)
if MQTT_USER:
    mqtt_client.username_pw_set(MQTT_USER, MQTT_PASS)

mqtt_client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
mqtt_client.loop_start()   # 后台线程维护心跳


def send_mqtt_cmd(device_id: str, action: str, msg_text: str = ""):
    """
    通过 MQTT 给指定座位下发控制指令
    Topic: netbar/<device_id>/cmd
    Payload:
        pc_on / pc_off
        light_on / light_off
        checkout        下机结算
        reset           远程复位
        msg:<文本>      发送短消息（这里支持中文，会转成 GB2312）
    """
    topic = f"netbar/{device_id}/cmd"

    # 普通控制命令都是 ASCII 文本
    if action in ("pc_on", "pc_off", "light_on", "light_off", "checkout", "reset"):
        payload = action
        mqtt_client.publish(topic, payload, qos=0)
        return

    # 短消息：把中文转成 GB2312 字节发送
    if action == "msg":
        msg_text = (msg_text or "").strip()
        if not msg_text:
            return
        try:
            prefix = b"msg:"
            msg_bytes = msg_text.encode("gb2312", errors="ignore")
            payload_bytes = prefix + msg_bytes
            mqtt_client.publish(topic, payload_bytes, qos=0)
        except Exception as e:
            print("send_mqtt_cmd msg error:", e)
        return


# ========= Flask 应用 =========

app = Flask(__name__)


@app.route("/")
def index():
    seats = load_seats()
    return render_template("index.html", seats=seats)


@app.route("/seat/<device_id>/<action>")
def seat_action(device_id, action):
    """
    远程控制：
      /seat/seat001/pc_on
      /seat/seat001/pc_off
      /seat/seat001/light_on
      /seat/seat001/light_off
      /seat/seat001/checkout
      /seat/seat001/reset
      /seat/seat001/maint_on
      /seat/seat001/maint_off
    """
    if action in ("maint_on", "maint_off"):
        # 更新维护状态到 devices 表
        conn = get_db_connection()
        try:
            with conn.cursor() as cur:
                cur.execute(
                    """
                    UPDATE devices
                       SET is_maintenance=%s
                     WHERE device_id=%s
                    """,
                    (1 if action == "maint_on" else 0, device_id),
                )
        finally:
            conn.close()

    send_mqtt_cmd(device_id, action)
    return redirect(url_for("index"))


@app.route("/msg/<device_id>", methods=["POST"])
def seat_send_msg(device_id):
    """
    向某个座位发一条短消息
    """
    text = request.form.get("msg", "")
    send_mqtt_cmd(device_id, "msg", text)
    return redirect(url_for("index"))


# ========= 广播消息 =========

@app.route("/broadcast", methods=["GET", "POST"])
def broadcast():
    """
    /broadcast 页面：
      - GET: 显示表单
      - POST: 向全部终端广播一条消息
    """
    if request.method == "POST":
        text = request.form.get("msg", "").strip()
        if text:
            # 查询所有设备 ID
            conn = get_db_connection()
            device_ids = []
            try:
                with conn.cursor() as cur:
                    cur.execute("SELECT device_id FROM devices ORDER BY device_id")
                    rows = cur.fetchall()
                    device_ids = [r["device_id"] for r in rows]
            finally:
                conn.close()

            for did in device_ids:
                send_mqtt_cmd(did, "msg", text)

            # 记录广播日志
            conn = get_db_connection()
            try:
                with conn.cursor() as cur:
                    cur.execute(
                        """
                        INSERT INTO broadcast_log (scope, device_id, text)
                        VALUES (%s, NULL, %s)
                        """,
                        ("all", text),
                    )
            finally:
                conn.close()

        return redirect(url_for("index"))

    return render_template("broadcast.html")


# ========= 用户管理 =========

@app.route("/users")
def users_list():
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute(
                """
                SELECT id, card_uid, username, birthdate, balance, is_active,
                       created_at, updated_at
                  FROM users
              ORDER BY id DESC
                """
            )
            rows = cur.fetchall()
    finally:
        conn.close()
    return render_template("users_list.html", users=rows)


@app.route("/users/new", methods=["GET", "POST"])
def users_new():
    if request.method == "POST":
        card_uid = request.form.get("card_uid", "").strip()
        username = request.form.get("username", "").strip()
        birthdate = request.form.get("birthdate", "").strip()  # yyyy-mm-dd
        balance = float(request.form.get("balance", "0") or 0)

        conn = get_db_connection()
        try:
            with conn.cursor() as cur:
                cur.execute(
                    """
                    INSERT INTO users (card_uid, username, birthdate, balance)
                    VALUES (%s, %s, %s, %s)
                    """,
                    (card_uid, username, birthdate, balance),
                )
        finally:
            conn.close()
        return redirect(url_for("users_list"))

    return render_template("users_edit.html", user=None)


@app.route("/users/<int:user_id>/edit", methods=["GET", "POST"])
def users_edit(user_id):
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM users WHERE id=%s", (user_id,))
            user = cur.fetchone()
    finally:
        conn.close()

    if not user:
        return "用户不存在", 404

    if request.method == "POST":
        username = request.form.get("username", "").strip()
        birthdate = request.form.get("birthdate", "").strip()
        is_active = 1 if request.form.get("is_active") == "on" else 0

        conn = get_db_connection()
        try:
            with conn.cursor() as cur:
                cur.execute(
                    """
                    UPDATE users
                       SET username=%s,
                           birthdate=%s,
                           is_active=%s
                     WHERE id=%s
                    """,
                    (username, birthdate, is_active, user_id),
                )
        finally:
            conn.close()
        return redirect(url_for("users_list"))

    return render_template("users_edit.html", user=user)


@app.route("/users/<int:user_id>/delete")
def users_delete(user_id):
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("DELETE FROM users WHERE id=%s", (user_id,))
    finally:
        conn.close()
    return redirect(url_for("users_list"))


@app.route("/users/<int:user_id>/recharge", methods=["GET", "POST"])
def users_recharge(user_id):
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM users WHERE id=%s", (user_id,))
            user = cur.fetchone()
    finally:
        conn.close()

    if not user:
        return "用户不存在", 404

    if request.method == "POST":
        amount = float(request.form.get("amount", "0") or 0)
        operator = request.form.get("operator", "").strip()
        remark   = request.form.get("remark", "").strip()

        conn = get_db_connection()
        try:
            with conn.cursor() as cur:
                # 更新余额
                new_balance = float(user["balance"]) + amount
                cur.execute(
                    "UPDATE users SET balance=%s WHERE id=%s",
                    (new_balance, user_id),
                )
                # 写充值流水
                cur.execute(
                    """
                    INSERT INTO recharge_log (user_id, amount, balance_after, operator, remark)
                    VALUES (%s, %s, %s, %s, %s)
                    """,
                    (user_id, amount, new_balance, operator, remark),
                )
        finally:
            conn.close()
        return redirect(url_for("users_list"))

    return render_template("users_recharge.html", user=user)


@app.route("/users/<int:user_id>/detail")
def users_detail(user_id):
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            # 用户信息
            cur.execute("SELECT * FROM users WHERE id=%s", (user_id,))
            user = cur.fetchone()

            if not user:
                return "用户不存在", 404

            # 最近上机记录
            cur.execute(
                """
                SELECT * FROM sessions
                 WHERE user_id=%s
              ORDER BY start_time DESC
                 LIMIT 50
                """,
                (user_id,),
            )
            sessions = cur.fetchall()

            # 最近充值记录
            cur.execute(
                """
                SELECT * FROM recharge_log
                 WHERE user_id=%s
              ORDER BY created_at DESC
                 LIMIT 50
                """,
                (user_id,),
            )
            recharges = cur.fetchall()

            # 最近扣费记录
            cur.execute(
                """
                SELECT * FROM consume_log
                 WHERE user_id=%s
              ORDER BY created_at DESC
                 LIMIT 50
                """,
                (user_id,),
            )
            consumes = cur.fetchall()

    finally:
        conn.close()

    return render_template(
        "users_detail.html",
        user=user,
        sessions=sessions,
        recharges=recharges,
        consumes=consumes,
    )


# ========= 日志 & 报表 =========

@app.route("/logs/sessions")
def logs_sessions():
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute(
                """
                SELECT s.*, u.username
                  FROM sessions s
             LEFT JOIN users u ON s.user_id = u.id
              ORDER BY s.start_time DESC
                 LIMIT 200
                """
            )
            rows = cur.fetchall()
    finally:
        conn.close()
    return render_template("logs_sessions.html", sessions=rows)


@app.route("/logs/alarms")
def logs_alarms():
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute(
                """
                SELECT * FROM alarm_log
              ORDER BY created_at DESC
                 LIMIT 200
                """
            )
            rows = cur.fetchall()
    finally:
        conn.close()
    return render_template("logs_alarms.html", alarms=rows)


@app.route("/report/revenue_daily")
def report_revenue_daily():
    """最近 30 天每日收入"""
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute(
                """
                SELECT DATE(start_time) AS d, SUM(fee) AS total_fee,
                       COUNT(*) AS cnt
                  FROM sessions
                 WHERE start_time >= DATE_SUB(CURDATE(), INTERVAL 30 DAY)
              GROUP BY DATE(start_time)
              ORDER BY d
                """
            )
            rows = cur.fetchall()
    finally:
        conn.close()

    labels   = [row["d"].strftime("%Y-%m-%d") for row in rows]
    data_fee = [float(row["total_fee"] or 0) for row in rows]
    data_cnt = [int(row["cnt"] or 0) for row in rows]

    return render_template(
        "report_revenue_daily.html",
        labels=labels,
        data_fee=data_fee,
        data_cnt=data_cnt,
    )


if __name__ == "__main__":
    # 对外开放 0.0.0.0:5000
    app.run(host="0.0.0.0", port=5000, debug=False)
