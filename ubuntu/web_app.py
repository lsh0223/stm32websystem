#!/usr/bin/env python3
# -*- coding: utf-8 -*-

from flask import Flask, render_template, redirect, url_for, request, jsonify, flash
import pymysql
import paho.mqtt.client as mqtt
from datetime import datetime
import time
import threading
# 登录认证相关库
from flask_login import LoginManager, UserMixin, login_user, login_required, logout_user, current_user
from werkzeug.security import generate_password_hash, check_password_hash

# ==========================================
#  配置区域
# ==========================================
MQTT_BROKER = "127.0.0.1" 
MQTT_PORT   = 1883
MQTT_CLIENT_ID = "web_admin_interface"
MQTT_LISTENER_ID = "web_background_listener" 
MQTT_USER   = ""
MQTT_PASS   = ""

DB_HOST = "127.0.0.1"
DB_PORT = 3306
DB_USER = "root"
DB_PASS = "123456"
DB_NAME = "netbar"

OFFLINE_SECS = 8 

app = Flask(__name__) 
# 必须设置密钥用于Session加密
app.secret_key = 'super_secret_key_for_netbar_system_lsh0223'

# ==========================================
#  Flask-Login 初始化配置
# ==========================================
login_manager = LoginManager()
login_manager.init_app(app)
login_manager.login_view = 'login'
login_manager.login_message = '请先登录系统后操作'
login_manager.login_message_category = 'warning'

class AdminUser(UserMixin):
    def __init__(self, id, username, password_hash):
        self.id = id
        self.username = username
        self.password_hash = password_hash

@login_manager.user_loader
def load_user(user_id):
    conn = get_db_connection()
    user = None
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM admins WHERE id=%s", (user_id,))
            row = cur.fetchone()
            if row:
                user = AdminUser(row['id'], row['username'], row['password_hash'])
    except Exception as e:
        print(f"User Loader Error: {e}")
    finally:
        conn.close()
    return user

# ==========================================
#  数据库连接助手
# ==========================================
def get_db_connection():
    return pymysql.connect(
        host=DB_HOST, port=DB_PORT, user=DB_USER, password=DB_PASS, database=DB_NAME,
        charset="utf8mb4", autocommit=True, cursorclass=pymysql.cursors.DictCursor
    )

def init_db_admin():
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("""
                CREATE TABLE IF NOT EXISTS admins (
                    id INT AUTO_INCREMENT PRIMARY KEY,
                    username VARCHAR(50) UNIQUE NOT NULL,
                    password_hash VARCHAR(255) NOT NULL,
                    last_login DATETIME
                )
            """)
            cur.execute("SELECT COUNT(*) as cnt FROM admins")
            res = cur.fetchone()
            if res['cnt'] == 0:
                default_pass = generate_password_hash("admin123")
                cur.execute("INSERT INTO admins (username, password_hash) VALUES (%s, %s)", 
                            ('admin', default_pass))
                print("★ 系统初始化：已创建默认管理员账号 admin / admin123")
    finally:
        conn.close()

# ==========================================
#  MQTT 发送助手
# ==========================================
def send_mqtt_cmd(device_id, action, msg_text=""):
    try:
        client = mqtt.Client(client_id=f"web_cmd_{int(time.time())}_{device_id}")
        if MQTT_USER: client.username_pw_set(MQTT_USER, MQTT_PASS)
        client.connect(MQTT_BROKER, MQTT_PORT, 60)
        
        topic = f"netbar/{device_id}/cmd"
        
        if action == "msg":
            try:
                payload = b"msg:" + msg_text.encode("gbk", errors="ignore")
                client.publish(topic, payload, qos=0)
            except:
                client.publish(topic, f"msg:{msg_text}", qos=0)
        else:
            client.publish(topic, action, qos=0)
            
        client.disconnect()
    except Exception as e:
        print(f"MQTT Send Error: {e}")

# ==========================================
#  后台 MQTT 监听线程
# ==========================================
def on_mqtt_message(client, userdata, msg):
    try:
        topic = msg.topic
        payload = msg.payload.decode('utf-8', errors='ignore')
        parts = topic.split('/')
        
        if len(parts) == 3 and parts[2] == 'alert':
            device_id = parts[1]
            alert_content = payload
            alert_type_code = "UNKNOWN"
            if "smoke" in alert_content or "fire" in alert_content:
                alert_type_code = "SMOKE" 
            elif "occupy" in alert_content:
                alert_type_code = "OCCUPY"
            
            conn = get_db_connection()
            with conn.cursor() as cur:
                cur.execute("INSERT INTO alarm_log (device_id, alarm_type, message, is_resolved, created_at) VALUES (%s, %s, %s, 0, NOW())", 
                            (device_id, alert_type_code, alert_content))
                cur.execute("UPDATE devices SET current_status=2, last_update=NOW() WHERE device_id=%s", (device_id,))
            conn.close()

        elif len(parts) == 3 and parts[2] == 'state':
            device_id = parts[1]
            conn = get_db_connection()
            with conn.cursor() as cur:
                cur.execute("UPDATE devices SET last_update=NOW() WHERE device_id=%s", (device_id,))
            conn.close()

    except Exception as e:
        print(f"MQTT Listener Error: {e}")

def start_mqtt_listener():
    def run_loop():
        client = mqtt.Client(client_id=MQTT_LISTENER_ID)
        if MQTT_USER: client.username_pw_set(MQTT_USER, MQTT_PASS)
        try:
            client.connect(MQTT_BROKER, MQTT_PORT, 60)
            client.subscribe("netbar/+/alert") 
            client.subscribe("netbar/+/state")
            client.on_message = on_mqtt_message
            print("🚀 后台 MQTT 监听线程已启动...")
            client.loop_forever()
        except Exception as e:
            print(f"❌ MQTT 监听启动失败 ({e}), 5秒后重试...")
            time.sleep(5) 
            run_loop()

    t = threading.Thread(target=run_loop, daemon=True)
    t.start()

# ==========================================
#  登录路由与逻辑
# ==========================================

@app.route("/login", methods=["GET", "POST"])
def login():
    if current_user.is_authenticated:
        return redirect(url_for('index'))

    if request.method == "POST":
        username = request.form.get("username")
        password = request.form.get("password")
        
        conn = get_db_connection()
        try:
            with conn.cursor() as cur:
                cur.execute("SELECT * FROM admins WHERE username=%s", (username,))
                admin_data = cur.fetchone()
                
            if admin_data and check_password_hash(admin_data['password_hash'], password):
                user = AdminUser(admin_data['id'], admin_data['username'], admin_data['password_hash'])
                login_user(user)
                flash('登录成功，欢迎回来！', 'success')
                
                with conn.cursor() as cur:
                    cur.execute("UPDATE admins SET last_login=NOW() WHERE id=%s", (admin_data['id'],))
                
                next_page = request.args.get('next')
                return redirect(next_page or url_for('index'))
            else:
                flash('用户名或密码错误', 'danger')
        finally:
            conn.close()
            
    return render_template("login.html")

@app.route("/logout")
@login_required
def logout():
    logout_user()
    flash('您已安全退出系统', 'info')
    return redirect(url_for('login'))

# ==========================================
#  受保护的 Web 页面路由
# ==========================================

@app.route("/")
@login_required
def index():
    return render_template("home.html", admin_name=current_user.username)

@app.route("/settings")
@login_required
def settings():
    return render_template("settings.html", admin_name=current_user.username)

@app.route("/report/revenue_daily")
@login_required
def report_revenue_page():
    return render_template("report_revenue_daily.html", admin_name=current_user.username)

@app.route("/users")
@login_required
def users_list():
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM users ORDER BY id DESC")
            rows = cur.fetchall()
    finally: conn.close()
    return render_template("users_list.html", users=rows, admin_name=current_user.username)

@app.route("/users/new", methods=["GET", "POST"])
@login_required
def users_new():
    if request.method == "POST":
        card_uid = request.form.get("card_uid", "").strip()
        username = request.form.get("username", "").strip()
        id_card = request.form.get("id_card", "").strip()
        balance = float(request.form.get("balance", "0") or 0)
        birthdate = "2000-01-01"
        if len(id_card) == 18:
            birthdate = f"{id_card[6:10]}-{id_card[10:12]}-{id_card[12:14]}"
        conn = get_db_connection()
        try:
            with conn.cursor() as cur:
                cur.execute("INSERT INTO users (card_uid, username, id_card, birthdate, balance) VALUES (%s, %s, %s, %s, %s)", 
                            (card_uid, username, id_card, birthdate, balance))
        finally: conn.close()
        return redirect(url_for("users_list"))
    return render_template("users_edit.html", user=None, admin_name=current_user.username)

@app.route("/users/<int:user_id>/edit", methods=["GET", "POST"])
@login_required
def users_edit(user_id):
    conn = get_db_connection()
    try:
        if request.method == "POST":
            username = request.form.get("username", "").strip()
            id_card = request.form.get("id_card", "").strip()
            is_active = 1 if request.form.get("is_active") == "on" else 0
            birthdate = "2000-01-01"
            if len(id_card) == 18:
                birthdate = f"{id_card[6:10]}-{id_card[10:12]}-{id_card[12:14]}"
            with conn.cursor() as cur:
                cur.execute("UPDATE users SET username=%s, id_card=%s, birthdate=%s, is_active=%s WHERE id=%s", 
                            (username, id_card, birthdate, is_active, user_id))
            return redirect(url_for("users_list"))
        else:
            with conn.cursor() as cur:
                cur.execute("SELECT * FROM users WHERE id=%s", (user_id,))
                user = cur.fetchone()
            return render_template("users_edit.html", user=user, admin_name=current_user.username)
    finally: conn.close()

@app.route("/users/<int:user_id>/recharge", methods=["GET", "POST"])
@login_required
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
        return render_template("users_recharge.html", user=user, admin_name=current_user.username)
    finally:
        conn.close()

@app.route("/users/<int:user_id>/delete")
@login_required
def users_delete(user_id):
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("DELETE FROM users WHERE id=%s", (user_id,))
    finally: conn.close()
    return redirect(url_for("users_list"))

@app.route("/users/<int:user_id>/detail")
@login_required
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
            
        return render_template("users_detail.html", user=user, sessions=sessions, recharges=recharges, consumes=consumes, admin_name=current_user.username)
    finally: conn.close()

@app.route("/logs/sessions")
@login_required
def logs_sessions():
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT id, user_name AS username, device_id, card_uid, start_time, end_time, duration_sec, fee, end_reason FROM user_session_log ORDER BY start_time DESC LIMIT 200")
            rows = cur.fetchall()
    finally: conn.close()
    return render_template("logs_sessions.html", sessions=rows, admin_name=current_user.username)

@app.route("/logs/alarms")
@login_required
def logs_alarms():
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM alarm_log ORDER BY created_at DESC LIMIT 200")
            rows = cur.fetchall()
    finally: conn.close()
    return render_template("logs_alarms.html", alarms=rows, admin_name=current_user.username)

@app.route("/broadcast", methods=["GET", "POST"])
@login_required
def broadcast():
    conn = get_db_connection()
    try:
        if request.method == "POST":
            scope = request.form.get("scope", "all")
            target_device = request.form.get("device_id", "").strip()
            message = request.form.get("text", "") or request.form.get("content", "")
            message = message.strip()
            if not message: return "错误：广播消息内容不能为空！", 400
            with conn.cursor() as cur:
                cur.execute("INSERT INTO broadcast_log (scope, device_id, text, created_at) VALUES (%s, %s, %s, NOW())", 
                            (scope, target_device, message))
                target_list = []
                if scope == "all":
                    cur.execute("SELECT device_id FROM devices")
                    rows = cur.fetchall()
                    target_list = [r['device_id'] for r in rows]
                elif target_device:
                    target_list = [target_device]
            for did in target_list:
                send_mqtt_cmd(did, "msg", message)
            return redirect(url_for("broadcast"))
        else:
            with conn.cursor() as cur:
                cur.execute("SELECT * FROM broadcast_log ORDER BY created_at DESC LIMIT 50")
                logs = cur.fetchall()
                cur.execute("SELECT device_id FROM devices ORDER BY device_id ASC")
                devices = cur.fetchall()
            return render_template("broadcast.html", logs=logs, devices=devices, admin_name=current_user.username)
    finally: conn.close()

# ==========================================
#  API 接口功能区
# ==========================================

@app.route('/get_rate', methods=['GET'])
@login_required
def get_rate():
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT v FROM config WHERE k='price_per_min'")
            row = cur.fetchone()
            current_price = row['v'] if row else "1.0"
        return jsonify({"rate": current_price})
    finally: conn.close()

@app.route('/update_rate', methods=['POST'])
@login_required
def update_rate():
    data = request.get_json()
    new_price = data.get('rate')
    conn = get_db_connection()
    try:
        float_price = float(new_price)
        with conn.cursor() as cur:
            cur.execute("INSERT INTO config (k, v) VALUES ('price_per_min', %s) ON DUPLICATE KEY UPDATE v=%s", (new_price, new_price))
            cur.execute("SELECT device_id FROM devices")
            devices = cur.fetchall()
        cmd_str = f"set_rate;val={float_price:.2f}"
        for dev in devices:
            send_mqtt_cmd(dev['device_id'], cmd_str)
        return jsonify({"status": "success", "message": "费率已更新"})
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500
    finally: conn.close()

@app.route("/api/seats_status")
@login_required
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
                else: is_offline = True
                data[r['device_id']] = {
                    "status": r['current_status'], 
                    "maint": r['is_maintenance'],
                    "user": user_name,
                    "smoke": r['smoke_percent'],
                    "sec": r['current_sec'],
                    "fee": float(r['current_fee'] or 0),
                    "offline": is_offline,
                    "pc": r['pc_status'],
                    "light": r['light_status'],
                    "human": r['human_status'],
                    "seat_name": r['seat_name']
                }
            return jsonify(data)
    finally: conn.close()

@app.route("/api/cmd", methods=["POST"])
@login_required
def api_cmd():
    did = request.form.get("device_id")
    cmd = request.form.get("command")
    val = request.form.get("value", "")
    if not did or not cmd: return jsonify({"status": "error", "message": "参数缺失"}), 400
    conn = get_db_connection()
    try:
        if cmd == "maint_on":
            with conn.cursor() as cur: cur.execute("UPDATE devices SET is_maintenance=1, current_status=0 WHERE device_id=%s", (did,))
        elif cmd == "maint_off":
            with conn.cursor() as cur: cur.execute("UPDATE devices SET is_maintenance=0 WHERE device_id=%s", (did,))
        if cmd == "checkout":
             with conn.cursor() as cur:
                 cur.execute("SELECT id, start_time FROM user_session_log WHERE device_id=%s AND end_time IS NULL ORDER BY id DESC LIMIT 1", (did,))
                 session = cur.fetchone()
                 if session:
                     start_time = session['start_time']
                     if isinstance(start_time, str): start_time = datetime.strptime(start_time, "%Y-%m-%d %H:%M:%S")
                     now = datetime.now()
                     duration_sec = int((now - start_time).total_seconds())
                     cur.execute("SELECT v FROM config WHERE k='price_per_min'")
                     row = cur.fetchone()
                     rate = float(row['v']) if row else 1.0
                     fee = round((duration_sec / 60.0) * rate, 2)
                     cur.execute("UPDATE user_session_log SET end_time=%s, duration_sec=%s, fee=%s, end_reason='admin_stop' WHERE id=%s", (now, duration_sec, fee, session['id']))
                 cur.execute("UPDATE devices SET current_status=0, current_user_id=NULL, current_sec=0, current_fee=0 WHERE device_id=%s", (did,))
        send_mqtt_cmd(did, cmd, val)
        return jsonify({"status": "ok"})
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500
    finally: conn.close()

@app.route("/api/device/rename", methods=["POST"])
@login_required
def api_rename_device():
    did = request.form.get("device_id")
    new_name = request.form.get("name")
    if not did or not new_name: return jsonify({"status": "error", "message": "名称不能为空"}), 400
    conn = get_db_connection()
    try:
        with conn.cursor() as cur: cur.execute("UPDATE devices SET seat_name=%s WHERE device_id=%s", (new_name, did))
        return jsonify({"status": "ok"})
    except Exception as e: return jsonify({"status": "error", "message": str(e)}), 500
    finally: conn.close()

@app.route("/api/alarm/<int:alarm_id>/resolve", methods=["POST"])
@login_required
def resolve_alarm(alarm_id):
    conn = get_db_connection()
    try:
        with conn.cursor() as cur: cur.execute("UPDATE alarm_log SET is_resolved=1 WHERE id=%s", (alarm_id,))
        return jsonify({"status": "ok"})
    except Exception as e: return jsonify({"status": "error", "message": "数据库错误"}), 500
    finally: conn.close()

@app.route("/api/report/revenue")
@login_required
def api_report_revenue():
    mode = request.args.get("mode", "daily")
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            if mode == 'weekly':
                sql = """SELECT DATE_FORMAT(start_time, '%Y第%u周') as d, SUM(fee) as total_fee, COUNT(*) as cnt 
                         FROM user_session_log WHERE start_time >= DATE_SUB(CURDATE(), INTERVAL 12 WEEK) GROUP BY d ORDER BY d"""
            elif mode == 'monthly':
                sql = """SELECT DATE_FORMAT(start_time, '%Y-%m') as d, SUM(fee) as total_fee, COUNT(*) as cnt 
                         FROM user_session_log WHERE start_time >= DATE_SUB(CURDATE(), INTERVAL 12 MONTH) GROUP BY d ORDER BY d"""
            else:
                sql = """SELECT DATE(start_time) as d, SUM(fee) as total_fee, COUNT(*) as cnt 
                         FROM user_session_log WHERE start_time >= DATE_SUB(CURDATE(), INTERVAL 30 DAY) GROUP BY d ORDER BY d"""
            cur.execute(sql)
            rows = cur.fetchall()
            labels = [str(r['d']) for r in rows]
            data_fee = [float(r['total_fee'] or 0) for r in rows]
            data_cnt = [int(r['cnt'] or 0) for r in rows]
            return jsonify({"labels": labels, "data_fee": data_fee, "data_cnt": data_cnt})
    finally: conn.close()

@app.route("/api/report/occupancy")
@login_required
def api_report_occupancy():
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT start_time, end_time FROM user_session_log WHERE start_time >= DATE_SUB(NOW(), INTERVAL 30 DAY)")
            rows = cur.fetchall()
        hour_counts = [0] * 24
        for r in rows:
            s = r['start_time']
            if not s: continue
            e = r['end_time'] if r['end_time'] else datetime.now()
            start_h = s.hour
            duration_hours = int((e - s).total_seconds() / 3600) + 1
            if duration_hours > 24: duration_hours = 24 
            for i in range(duration_hours):
                hour_counts[(start_h + i) % 24] += 1
        avg_occupancy = [round(c / 30.0, 1) for c in hour_counts]
        return jsonify({"hours": [str(i) for i in range(24)], "avg_occupancy": avg_occupancy})
    finally: conn.close()

if __name__ == "__main__":
    init_db_admin() # 初始化数据库
    start_mqtt_listener()
    print("🚀 智能无人网吧系统启动: http://localhost:5000")
    app.run(host="0.0.0.0", port=5000, debug=True, use_reloader=False)
    