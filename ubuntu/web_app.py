#!/usr/bin/env python3
# -*- coding: utf-8 -*-

from flask import Flask, render_template, redirect, url_for, request, jsonify
import pymysql
import paho.mqtt.client as mqtt
from datetime import datetime, date
import time

# ========= MQTT 配置 (本地) =========
MQTT_BROKER = "127.0.0.1" 
MQTT_PORT   = 1883
MQTT_CLIENT_ID = "web_admin_interface"
MQTT_USER   = ""
MQTT_PASS   = ""

# ========= MySQL 配置 =========
DB_HOST = "127.0.0.1"
DB_PORT = 3306
DB_USER = "root"
DB_PASS = "123456"
DB_NAME = "netbar"

OFFLINE_SECS = 5

app = Flask(__name__) # 默认使用 templates 文件夹

def get_db_connection():
    return pymysql.connect(
        host=DB_HOST, port=DB_PORT, user=DB_USER, password=DB_PASS, database=DB_NAME,
        charset="utf8mb4", autocommit=True, cursorclass=pymysql.cursors.DictCursor
    )

# MQTT 发送助手 (保持你原有的逻辑)
def send_mqtt_cmd(device_id, action, msg_text=""):
    try:
        client = mqtt.Client(client_id=f"web_cmd_{int(time.time())}_{device_id}")
        if MQTT_USER: client.username_pw_set(MQTT_USER, MQTT_PASS)
        client.connect(MQTT_BROKER, MQTT_PORT, 60)
        
        topic = f"netbar/{device_id}/cmd"
        
        if action == "msg":
            # 发送消息特殊处理
            try:
                payload = b"msg:" + msg_text.encode("gbk", errors="ignore")
                client.publish(topic, payload, qos=0)
            except:
                client.publish(topic, f"msg:{msg_text}", qos=0)
        else:
            # 普通指令: pc_on, checkout, maint_on, set_rate 等
            client.publish(topic, action, qos=0)
            
        client.disconnect()
    except Exception as e:
        print(f"MQTT Error: {e}")

# ================= 核心页面路由 =================

@app.route("/")
def index():
    # 首页直接显示监控大屏
    return render_template("home.html")

# ★★★ 修改：设置页面路由 (只负责显示页面) ★★★
@app.route("/settings")
def settings():
    # 这里只渲染页面，数据通过下面的 API 获取
    return render_template("settings.html")

# ★★★ 新增：获取当前费率 API (配合 settings.html) ★★★
@app.route('/get_rate', methods=['GET'])
def get_rate():
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            # 从数据库读取配置
            cur.execute("SELECT v FROM config WHERE k='price_per_min'")
            row = cur.fetchone()
            current_price = row['v'] if row else "1.0"
        return jsonify({"rate": current_price})
    except Exception as e:
        print(f"Error getting rate: {e}")
        return jsonify({"rate": "1.0"})
    finally:
        conn.close()

# ★★★ 新增：更新费率 API (配合 settings.html) ★★★
@app.route('/update_rate', methods=['POST'])
def update_rate():
    # 1. 获取前端 JSON 数据
    data = request.get_json()
    new_price = data.get('rate')
    
    if not new_price:
        return jsonify({"status": "error", "message": "费率不能为空"}), 400

    conn = get_db_connection()
    try:
        # 2. 转换数据类型
        float_price = float(new_price)
        
        # 3. 更新数据库 (config 表)
        with conn.cursor() as cur:
            cur.execute("INSERT INTO config (k, v) VALUES ('price_per_min', %s) ON DUPLICATE KEY UPDATE v=%s", (new_price, new_price))
        
        # 4. 广播给所有设备
        with conn.cursor() as cur:
            cur.execute("SELECT device_id FROM devices")
            devices = cur.fetchall()
            
        # 构造指令: set_rate;val=2.50
        cmd_str = f"set_rate;val={float_price:.2f}"
        
        print(f"正在广播新费率: {cmd_str} 到 {len(devices)} 台设备...")
        for dev in devices:
            send_mqtt_cmd(dev['device_id'], cmd_str)
            
        return jsonify({"status": "success", "message": "费率已更新并下发"})
        
    except ValueError:
        return jsonify({"status": "error", "message": "费率格式错误"}), 400
    except Exception as e:
        print(f"Error updating rate: {e}")
        return jsonify({"status": "error", "message": str(e)}), 500
    finally:
        conn.close()

# ★★★ 前端轮询接口 (保持不变) ★★★
@app.route("/api/seats_status")
def api_seats_status():
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM devices")
            rows = cur.fetchall()
            
            data = {}
            now = datetime.now()
            
            for r in rows:
                user_name = "--"
                if r['current_user_id']:
                    cur.execute("SELECT username FROM users WHERE id=%s", (r['current_user_id'],))
                    u = cur.fetchone()
                    if u: user_name = u['username']
                
                is_offline = False
                if r['last_update']:
                    delta = (now - r['last_update']).total_seconds()
                    if delta > OFFLINE_SECS: is_offline = True
                else:
                    is_offline = True

                data[r['device_id']] = {
                    "status": r['current_status'], # 0=空闲, 1=使用, 2=报警
                    "maint": r['is_maintenance'],
                    "user": user_name,
                    "smoke": r['smoke_percent'],
                    "sec": r['current_sec'],
                    "fee": float(r['current_fee'] or 0),
                    "offline": is_offline,
                    "pc": r['pc_status'],      # 电脑状态
                    "light": r['light_status'],# 灯状态
                    "human": r['human_status'] # 红外人体状态
                }
            return jsonify(data)
    finally: conn.close()

# ★★★ 前端控制接口 (保持不变) ★★★
@app.route("/api/cmd", methods=["POST"])
def api_cmd():
    did = request.form.get("device_id")
    cmd = request.form.get("command")
    val = request.form.get("value", "") # 用于消息内容
    
    if not did or not cmd: return jsonify({"status": "err"}), 400

    # 维护模式特殊处理：立即更新数据库，防止网页刷新后状态不对
    if cmd == "maint_on":
        conn = get_db_connection()
        with conn.cursor() as cur:
            cur.execute("UPDATE devices SET is_maintenance=1, current_status=0 WHERE device_id=%s", (did,))
        conn.close()
    elif cmd == "maint_off":
        conn = get_db_connection()
        with conn.cursor() as cur:
            cur.execute("UPDATE devices SET is_maintenance=0 WHERE device_id=%s", (did,))
        conn.close()
        
    send_mqtt_cmd(did, cmd, val)
    return jsonify({"status": "ok"})

# ================= 原有管理功能 (全部保留) =================

@app.route("/users")
def users_list():
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM users ORDER BY id DESC")
            rows = cur.fetchall()
    finally: conn.close()
    return render_template("users_list.html", users=rows)

@app.route("/users/new", methods=["GET", "POST"])
def users_new():
    if request.method == "POST":
        card_uid = request.form.get("card_uid", "").strip()
        username = request.form.get("username", "").strip()
        id_card = request.form.get("id_card", "").strip()
        balance = float(request.form.get("balance", "0") or 0)
        
        # 简单生日解析
        birthdate = "2000-01-01"
        if len(id_card) == 18:
            b = id_card[6:14]
            birthdate = f"{b[0:4]}-{b[4:6]}-{b[6:8]}"

        conn = get_db_connection()
        try:
            with conn.cursor() as cur:
                cur.execute("INSERT INTO users (card_uid, username, id_card, birthdate, balance) VALUES (%s, %s, %s, %s, %s)", 
                            (card_uid, username, id_card, birthdate, balance))
        finally: conn.close()
        return redirect(url_for("users_list"))
    return render_template("users_edit.html", user=None)

@app.route("/users/<int:user_id>/edit", methods=["GET", "POST"])
def users_edit(user_id):
    conn = get_db_connection()
    try:
        if request.method == "POST":
            username = request.form.get("username", "").strip()
            id_card = request.form.get("id_card", "").strip()
            is_active = 1 if request.form.get("is_active") == "on" else 0
            
            birthdate = "2000-01-01"
            if len(id_card) == 18:
                b = id_card[6:14]
                birthdate = f"{b[0:4]}-{b[4:6]}-{b[6:8]}"

            with conn.cursor() as cur:
                cur.execute("UPDATE users SET username=%s, id_card=%s, birthdate=%s, is_active=%s WHERE id=%s", 
                            (username, id_card, birthdate, is_active, user_id))
            return redirect(url_for("users_list"))
        else:
            with conn.cursor() as cur:
                cur.execute("SELECT * FROM users WHERE id=%s", (user_id,))
                user = cur.fetchone()
            return render_template("users_edit.html", user=user)
    finally: conn.close()

@app.route("/users/<int:user_id>/recharge", methods=["GET", "POST"])
def users_recharge(user_id):
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM users WHERE id=%s", (user_id,))
            user = cur.fetchone()
            
        if request.method == "POST":
            amount = float(request.form.get("amount", "0") or 0)
            with conn.cursor() as cur:
                new_bal = float(user["balance"]) + amount
                cur.execute("UPDATE users SET balance=%s WHERE id=%s", (new_bal, user_id))
                cur.execute("INSERT INTO recharge_log (user_id, amount, balance_after, created_at) VALUES (%s, %s, %s, NOW())", 
                            (user_id, amount, new_bal))
            return redirect(url_for("users_list"))
            
        return render_template("users_recharge.html", user=user)
    finally: conn.close()

@app.route("/users/<int:user_id>/delete")
def users_delete(user_id):
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("DELETE FROM users WHERE id=%s", (user_id,))
    finally: conn.close()
    return redirect(url_for("users_list"))

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
            
        return render_template("users_detail.html", user=user, sessions=sessions, recharges=recharges, consumes=consumes)
    finally: conn.close()

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
            cur.execute("""SELECT DATE(start_time) AS d, SUM(fee) AS total_fee, COUNT(*) AS cnt 
                           FROM user_session_log 
                           WHERE start_time >= DATE_SUB(CURDATE(), INTERVAL 30 DAY) 
                           GROUP BY DATE(start_time) 
                           ORDER BY d""")
            rows = cur.fetchall()
    finally: conn.close()
    
    labels   = [row["d"].strftime("%Y-%m-%d") for row in rows]
    data_fee = [float(row["total_fee"] or 0) for row in rows]
    data_cnt = [int(row["cnt"] or 0) for row in rows]
    
    return render_template("report_revenue_daily.html", labels=labels, data_fee=data_fee, data_cnt=data_cnt)

@app.route("/broadcast", methods=["GET", "POST"])
def broadcast():
    conn = get_db_connection()
    try:
        if request.method == "POST":
            scope = request.form.get("scope", "all")
            target_device = request.form.get("device_id", "").strip()
            message = request.form.get("text", "") or request.form.get("content", "")
            message = message.strip()
            
            if not message:
                return "错误：广播消息内容不能为空！", 400

            # 1. 写入数据库
            with conn.cursor() as cur:
                cur.execute("INSERT INTO broadcast_log (scope, device_id, text, created_at) VALUES (%s, %s, %s, NOW())", 
                            (scope, target_device, message))
                
                # 2. 确定发送目标
                target_list = []
                if scope == "all":
                    cur.execute("SELECT device_id FROM devices")
                    rows = cur.fetchall()
                    target_list = [r['device_id'] for r in rows]
                elif target_device:
                    target_list = [target_device]
            
            # 3. 发送 MQTT
            for did in target_list:
                send_mqtt_cmd(did, "msg", message)
                
            return redirect(url_for("broadcast"))

        else:
            with conn.cursor() as cur:
                cur.execute("SELECT * FROM broadcast_log ORDER BY created_at DESC LIMIT 50")
                logs = cur.fetchall()
                cur.execute("SELECT device_id FROM devices ORDER BY device_id ASC")
                devices = cur.fetchall()
            return render_template("broadcast.html", logs=logs, devices=devices)
    finally:
        conn.close()

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=True)
    