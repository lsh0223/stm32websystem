#!/usr/bin/env python3
# -*- coding: utf-8 -*-

from flask import Flask, render_template, redirect, url_for, request, jsonify
import pymysql
import paho.mqtt.client as mqtt
from datetime import datetime, date

# ========= MQTT 配置 (本地) =========
MQTT_BROKER = "127.0.0.1" 
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

OFFLINE_SECS = 60

def get_db_connection():
    return pymysql.connect(
        host=DB_HOST, port=DB_PORT, user=DB_USER, password=DB_PASS, database=DB_NAME,
        charset="utf8mb4", autocommit=True, cursorclass=pymysql.cursors.DictCursor
    )

def load_seats():
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute(
                """
                SELECT d.device_id, d.seat_name, d.current_status,
                       d.is_maintenance, d.pc_status, d.light_status, d.human_status,
                       d.smoke_percent, d.current_sec, d.current_fee,
                       d.last_update,
                       u.username AS current_user_name,
                       u.card_uid AS current_card_uid
                  FROM devices d
             LEFT JOIN users u ON d.current_user_id = u.id
              ORDER BY d.device_id
                """
            )
            rows = cur.fetchall()
    finally: conn.close()

    now = datetime.now()
    for r in rows:
        last = r["last_update"]
        is_maint = bool(r.get("is_maintenance", 0))
        r["pc_text"]    = "开机" if r["pc_status"] else "关机"
        r["light_text"] = "开启" if r["light_status"] else "关闭"
        r["human_text"] = "有人" if r["human_status"] else "无人"
        r["duration_min"] = (r.get("current_sec", 0) or 0) // 60 # 预计算分钟

        if r.get("current_user_name"):
            r["seat_name_display"] = f"{r['seat_name']} - {r['current_user_name']}"
            r["user_info_display"] = f"卡号: {r['current_card_uid']}"
        else:
            r["seat_name_display"] = r['seat_name']
            r["user_info_display"] = "无用户"

        sm = r["smoke_percent"] or 0
        if sm < 20: r["smoke_level"] = "良好"
        elif sm < 40: r["smoke_level"] = "中等"
        elif sm < 60: r["smoke_level"] = "偏高"
        else: r["smoke_level"] = "危险"

        is_offline = False
        if last is None: is_offline = True
        elif isinstance(last, datetime):
            if (now - last).total_seconds() > OFFLINE_SECS: is_offline = True
        r["is_offline"]     = is_offline
        r["is_maintenance"] = is_maint

        if is_offline:
            r["status_text"] = "离线"; r["status_class"] = "badge bg-dark"
        elif is_maint:
            r["status_text"] = "维护中"; r["status_class"] = "badge bg-info"
        else:
            cs = r["current_status"] or 0
            if cs == 1: r["status_text"] = "上机中"; r["status_class"] = "badge bg-success"
            elif cs == 2: r["status_text"] = "告警"; r["status_class"] = "badge bg-danger"
            else: r["status_text"] = "空闲"; r["status_class"] = "badge bg-secondary"

        fee = r["current_fee"] or 0
        r["current_fee_fmt"] = f"{fee:.2f}"
    return rows

mqtt_client = mqtt.Client(client_id=MQTT_CLIENT_ID, clean_session=True)
if MQTT_USER: mqtt_client.username_pw_set(MQTT_USER, MQTT_PASS)
try:
    mqtt_client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    mqtt_client.loop_start()
except Exception as e: print(f"Warning: MQTT connect failed: {e}")

def send_mqtt_cmd(device_id: str, action: str, msg_text: str = ""):
    topic = f"netbar/{device_id}/cmd"
    if action in ("pc_on", "pc_off", "light_on", "light_off", "checkout", "reset"):
        mqtt_client.publish(topic, action, qos=0)
        return
    if action == "msg":
        msg_text = (msg_text or "").strip()
        if not msg_text: return
        try:
            prefix = b"msg:"
            msg_bytes = msg_text.encode("gbk", errors="ignore")
            mqtt_client.publish(topic, prefix + msg_bytes, qos=0)
        except Exception as e: print("send_mqtt_cmd msg error:", e)
        return

app = Flask(__name__)

# ★★★ 修改：根路由只返回外壳 ★★★
@app.route("/")
def index():
    return render_template("index.html")

# ★★★ 新增：监控页内容路由 ★★★
@app.route("/home")
def home():
    seats = load_seats()
    return render_template("home.html", seats=seats)

@app.route("/api/seats")
def api_seats():
    seats = load_seats()
    for r in seats:
        if isinstance(r.get("last_update"), datetime):
            r["last_update"] = r["last_update"].strftime("%Y-%m-%d %H:%M:%S")
        elif r.get("last_update") is None:
            r["last_update"] = "未知"
    return jsonify(seats)

@app.route("/seat/<device_id>/<action>")
def seat_action(device_id, action):
    if action in ("maint_on", "maint_off"):
        conn = get_db_connection()
        try:
            with conn.cursor() as cur:
                cur.execute("UPDATE devices SET is_maintenance=%s WHERE device_id=%s", (1 if action == "maint_on" else 0, device_id))
        finally: conn.close()
    send_mqtt_cmd(device_id, action)
    # 动作完成后，重定向回监控内容页
    return redirect(url_for("home"))

@app.route("/msg/<device_id>", methods=["POST"])
def seat_send_msg(device_id):
    text = request.form.get("msg", "")
    send_mqtt_cmd(device_id, "msg", text)
    return redirect(url_for("home"))

@app.route("/broadcast", methods=["GET", "POST"])
def broadcast():
    if request.method == "POST":
        text = request.form.get("msg", "").strip()
        if text:
            conn = get_db_connection()
            try:
                with conn.cursor() as cur:
                    cur.execute("SELECT device_id FROM devices ORDER BY device_id")
                    for r in cur.fetchall(): send_mqtt_cmd(r["device_id"], "msg", text)
            finally: conn.close()
        return redirect(url_for("home")) # 广播后回首页
    return render_template("broadcast.html")

@app.route("/users")
def users_list():
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM users ORDER BY id DESC")
            rows = cur.fetchall()
    finally: conn.close()
    return render_template("users_list.html", users=rows)

def get_birth_from_id(id_card):
    if len(id_card) == 18:
        birth_str = id_card[6:14]
        return f"{birth_str[0:4]}-{birth_str[4:6]}-{birth_str[6:8]}"
    return "2000-01-01"

@app.route("/users/new", methods=["GET", "POST"])
def users_new():
    if request.method == "POST":
        card_uid, username = request.form.get("card_uid", "").strip(), request.form.get("username", "").strip()
        id_card, balance = request.form.get("id_card", "").strip(), float(request.form.get("balance", "0") or 0)
        birthdate = get_birth_from_id(id_card)
        conn = get_db_connection()
        try:
            with conn.cursor() as cur:
                cur.execute("INSERT INTO users (card_uid, username, id_card, birthdate, balance) VALUES (%s, %s, %s, %s, %s)", (card_uid, username, id_card, birthdate, balance))
        finally: conn.close()
        return redirect(url_for("users_list"))
    return render_template("users_edit.html", user=None)

@app.route("/users/<int:user_id>/edit", methods=["GET", "POST"])
def users_edit(user_id):
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM users WHERE id=%s", (user_id,))
            user = cur.fetchone()
    finally: conn.close()
    if request.method == "POST":
        username, id_card = request.form.get("username", "").strip(), request.form.get("id_card", "").strip()
        birthdate = get_birth_from_id(id_card)
        is_active = 1 if request.form.get("is_active") == "on" else 0
        conn = get_db_connection()
        try:
            with conn.cursor() as cur:
                cur.execute("UPDATE users SET username=%s, id_card=%s, birthdate=%s, is_active=%s WHERE id=%s", (username, id_card, birthdate, is_active, user_id))
        finally: conn.close()
        return redirect(url_for("users_list"))
    return render_template("users_edit.html", user=user)

@app.route("/users/<int:user_id>/delete")
def users_delete(user_id):
    conn = get_db_connection()
    try:
        with conn.cursor() as cur: cur.execute("DELETE FROM users WHERE id=%s", (user_id,))
    finally: conn.close()
    return redirect(url_for("users_list"))

@app.route("/users/<int:user_id>/recharge", methods=["GET", "POST"])
def users_recharge(user_id):
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM users WHERE id=%s", (user_id,))
            user = cur.fetchone()
    finally: conn.close()
    if request.method == "POST":
        amount = float(request.form.get("amount", "0") or 0)
        conn = get_db_connection()
        try:
            with conn.cursor() as cur:
                new_balance = float(user["balance"]) + amount
                cur.execute("UPDATE users SET balance=%s WHERE id=%s", (new_balance, user_id))
                cur.execute("INSERT INTO recharge_log (user_id, amount, balance_after, created_at) VALUES (%s, %s, %s, NOW())", (user_id, amount, new_balance))
        finally: conn.close()
        return redirect(url_for("users_list"))
    return render_template("users_recharge.html", user=user)

@app.route("/users/<int:user_id>/detail")
def users_detail(user_id):
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM users WHERE id=%s", (user_id,))
            user = cur.fetchone()
            if not user: return "用户不存在", 404
            cur.execute("SELECT * FROM user_session_log WHERE user_name=%s ORDER BY start_time DESC LIMIT 50", (user['username'],))
            sessions = cur.fetchall()
            cur.execute("SELECT * FROM recharge_log WHERE user_id=%s ORDER BY created_at DESC LIMIT 50", (user_id,))
            recharges = cur.fetchall()
            cur.execute("SELECT * FROM consume_log WHERE user_id=%s ORDER BY created_at DESC LIMIT 50", (user_id,))
            consumes = cur.fetchall()
    finally: conn.close()
    return render_template("users_detail.html", user=user, sessions=sessions, recharges=recharges, consumes=consumes)

@app.route("/logs/sessions")
def logs_sessions():
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT id, user_name AS username, device_id, card_uid, start_time, end_time, duration_sec, fee, end_reason FROM user_session_log ORDER BY start_time DESC LIMIT 200")
            rows = cur.fetchall()
    finally: conn.close()
    return render_template("logs_sessions.html", sessions=rows)

@app.route("/logs/alarms")
def logs_alarms():
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM alarm_log ORDER BY created_at DESC LIMIT 200")
            rows = cur.fetchall()
    finally: conn.close()
    return render_template("logs_alarms.html", alarms=rows)

@app.route("/report/revenue_daily")
def report_revenue_daily():
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("""SELECT DATE(start_time) AS d, SUM(fee) AS total_fee, COUNT(*) AS cnt FROM user_session_log WHERE start_time >= DATE_SUB(CURDATE(), INTERVAL 30 DAY) GROUP BY DATE(start_time) ORDER BY d""")
            rows = cur.fetchall()
    finally: conn.close()
    labels   = [row["d"].strftime("%Y-%m-%d") for row in rows]
    data_fee = [float(row["total_fee"] or 0) for row in rows]
    data_cnt = [int(row["cnt"] or 0) for row in rows]
    return render_template("report_revenue_daily.html", labels=labels, data_fee=data_fee, data_cnt=data_cnt)

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=False)
    