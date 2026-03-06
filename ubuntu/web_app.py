#!/usr/bin/env python3
# -*- coding: utf-8 -*-

from flask import Flask, render_template, redirect, url_for, request, jsonify, flash, session
import pymysql
import paho.mqtt.client as mqtt
from datetime import datetime
import time
import threading
import random  # 新增：用于生成验证码
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
app.secret_key = 'super_secret_key_for_netbar_system_lsh0223'

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
        pass
    finally:
        conn.close()
    return user

def get_db_connection():
    return pymysql.connect(
        host=DB_HOST, port=DB_PORT, user=DB_USER, password=DB_PASS, database=DB_NAME,
        charset="utf8mb4", autocommit=True, cursorclass=pymysql.cursors.DictCursor
    )

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
#  用户门户路由
# ==========================================

@app.route("/portal")
def portal_index():
    if 'user_id' in session: return redirect(url_for('portal_dashboard'))
    return redirect(url_for('portal_login'))

@app.route("/portal/register", methods=["GET", "POST"])
def portal_register():
    if request.method == "POST":
        username = request.form.get("username", "").strip()
        password = request.form.get("password", "").strip()
        question = request.form.get("question", "").strip()
        answer = request.form.get("answer", "").strip()
        user_captcha = request.form.get("captcha", "").strip()

        # 校验验证码
        if 'captcha' not in session or user_captcha != session['captcha']:
            flash("验证码计算错误，请重试", "danger")
            return redirect(url_for('portal_register'))

        if not username or not password or not question or not answer:
            flash("所有字段不能为空", "danger")
            return redirect(url_for('portal_register'))
        
        conn = get_db_connection()
        try:
            with conn.cursor() as cur:
                cur.execute("SELECT id FROM users WHERE username=%s", (username,))
                if cur.fetchone():
                    flash("用户名已被注册", "danger")
                else:
                    pwd_hash = generate_password_hash(password)
                    cur.execute("INSERT INTO users (username, password_hash, security_question, security_answer, is_active) VALUES (%s, %s, %s, %s, 1)", 
                                (username, pwd_hash, question, answer))
                    flash("注册成功，请妥善保管您的密保答案！", "success")
                    session.pop('captcha', None)
                    return redirect(url_for('portal_login'))
        finally: conn.close()

    # GET 请求时生成简单的数学算术验证码
    num1 = random.randint(1, 10)
    num2 = random.randint(1, 10)
    session['captcha'] = str(num1 + num2)
    captcha_text = f"{num1} + {num2} = ?"
    return render_template("portal_register.html", captcha_text=captcha_text)

@app.route("/portal/login", methods=["GET", "POST"])
def portal_login():
    if request.method == "POST":
        username = request.form.get("username", "").strip()
        password = request.form.get("password", "").strip()
        conn = get_db_connection()
        try:
            with conn.cursor() as cur:
                cur.execute("SELECT * FROM users WHERE username=%s", (username,))
                user = cur.fetchone()
                if user and user['password_hash'] and check_password_hash(user['password_hash'], password):
                    if user['is_active'] == 0:
                        flash("账户已被禁用", "danger")
                    else:
                        session['user_id'] = user['id']
                        session['username'] = user['username']
                        flash("登录成功", "success")
                        return redirect(url_for('portal_dashboard'))
                else:
                    flash("用户名或密码错误", "danger")
        finally: conn.close()
    return render_template("portal_login.html")

@app.route("/portal/forgot_password", methods=["GET", "POST"])
def portal_forgot_password():
    step = request.form.get("step", "1")
    
    if request.method == "POST":
        # 步骤 1：输入用户名，查询是否存在密保问题
        if step == "1":
            username = request.form.get("username", "").strip()
            conn = get_db_connection()
            try:
                with conn.cursor() as cur:
                    cur.execute("SELECT security_question FROM users WHERE username=%s", (username,))
                    user = cur.fetchone()
                    if user and user['security_question']:
                        return render_template("portal_forgot_password.html", step="2", username=username, question=user['security_question'])
                    else:
                        flash("该用户不存在或未设置密保问题", "danger")
            finally: conn.close()
            
        # 步骤 2：校验密保答案并重置密码
        elif step == "2":
            username = request.form.get("username", "").strip()
            answer = request.form.get("answer", "").strip()
            new_password = request.form.get("new_password", "").strip()
            question_hidden = request.form.get("question_hidden", "")
            
            conn = get_db_connection()
            try:
                with conn.cursor() as cur:
                    cur.execute("SELECT security_answer FROM users WHERE username=%s", (username,))
                    user = cur.fetchone()
                    if user and user['security_answer'] == answer:
                        pwd_hash = generate_password_hash(new_password)
                        cur.execute("UPDATE users SET password_hash=%s WHERE username=%s", (pwd_hash, username))
                        flash("密码重置成功，请使用新密码登录", "success")
                        return redirect(url_for('portal_login'))
                    else:
                        flash("密保答案错误！", "danger")
                        return render_template("portal_forgot_password.html", step="2", username=username, question=question_hidden)
            finally: conn.close()

    # 默认渲染第一步验证用户名
    return render_template("portal_forgot_password.html", step="1")

@app.route("/portal/logout")
def portal_logout():
    session.pop('user_id', None)
    session.pop('username', None)
    flash("已安全退出", "success")
    return redirect(url_for('portal_login'))

@app.route("/portal/dashboard")
def portal_dashboard():
    if 'user_id' not in session: return redirect(url_for('portal_login'))
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM users WHERE id=%s", (session['user_id'],))
            user = cur.fetchone()
            
            if not user:
                session.pop('user_id', None)
                session.pop('username', None)
                flash("账号登录状态已失效或被删除，请重新登录", "danger")
                return redirect(url_for('portal_login'))

            total_rech = float(user['total_recharge'])
            level = "普通会员"
            discount = 1.0
            if total_rech >= 1000: level = "钻石会员"; discount = 0.9
            elif total_rech >= 500: level = "黄金会员"; discount = 0.93
            elif total_rech >= 300: level = "白银会员"; discount = 0.95
            elif total_rech >= 100: level = "青铜会员"; discount = 0.98

    finally: conn.close()
    return render_template("portal_dashboard.html", user=user, level=level, discount=discount)

@app.route("/portal/bind", methods=["GET", "POST"])
def portal_bind():
    if 'user_id' not in session: return redirect(url_for('portal_login'))
    if request.method == "POST":
        code = request.form.get("code", "").strip()
        id_card = request.form.get("id_card", "").strip()
        conn = get_db_connection()
        try:
            with conn.cursor() as cur:
                cur.execute("SELECT * FROM binding_codes WHERE code=%s AND id_card=%s ORDER BY id DESC LIMIT 1", (code, id_card))
                record = cur.fetchone()
                if record:
                    cur.execute("SELECT id FROM users WHERE card_uid=%s", (record['card_uid'],))
                    conflict = cur.fetchone()
                    if conflict and conflict['id'] != session['user_id']:
                        flash("该卡片已被其他账号绑定！", "danger")
                    else:
                        birthdate = "2000-01-01"
                        if len(id_card) == 18:
                            birthdate = f"{id_card[6:10]}-{id_card[10:12]}-{id_card[12:14]}"
                        cur.execute("UPDATE users SET card_uid=%s, id_card=%s, birthdate=%s WHERE id=%s", 
                                    (record['card_uid'], id_card, birthdate, session['user_id']))
                        cur.execute("DELETE FROM binding_codes WHERE code=%s", (code,))
                        flash("卡片及身份信息绑定成功！去网吧刷卡即可直接上机或开门。", "success")
                        return redirect(url_for('portal_dashboard'))
                else:
                    flash("绑定码错误或与身份证不匹配！", "danger")
        finally: conn.close()
    return render_template("portal_bind.html")

@app.route("/portal/recharge", methods=["GET", "POST"])
def portal_recharge():
    if 'user_id' not in session: return redirect(url_for('portal_login'))
    if request.method == "POST":
        amount = float(request.form.get("amount", 0))
        if amount > 0:
            conn = get_db_connection()
            try:
                with conn.cursor() as cur:
                    cur.execute("UPDATE users SET balance=balance+%s, total_recharge=total_recharge+%s WHERE id=%s", (amount, amount, session['user_id']))
                    cur.execute("SELECT balance FROM users WHERE id=%s", (session['user_id'],))
                    new_bal = cur.fetchone()['balance']
                    cur.execute("INSERT INTO recharge_log (user_id, amount, balance_after, remark, created_at) VALUES (%s, %s, %s, '用户自助充值', NOW())", 
                                (session['user_id'], amount, new_bal))
                flash(f"成功充值 {amount} 元，累计充值可升级会员！", "success")
            finally: conn.close()
    return render_template("portal_recharge.html")

@app.route("/portal/history")
def portal_history():
    if 'user_id' not in session: return redirect(url_for('portal_login'))
    conn = get_db_connection()
    try:
        with conn.cursor() as cur:
            sql = """
                SELECT c.amount, c.created_at, s.start_time, s.end_time, s.device_id, s.duration_sec
                FROM consume_log c
                LEFT JOIN user_session_log s ON c.session_id = s.id
                WHERE c.user_id=%s
                ORDER BY c.created_at DESC LIMIT 20
            """
            cur.execute(sql, (session['user_id'],))
            consumes = cur.fetchall()
            
            cur.execute("SELECT * FROM recharge_log WHERE user_id=%s ORDER BY created_at DESC LIMIT 20", (session['user_id'],))
            recharges = cur.fetchall()
    finally: conn.close()
    return render_template("portal_history.html", consumes=consumes, recharges=recharges)


# ==========================================
#  后台管理员页面及API保持原有完整功能
# ==========================================
@app.route("/login", methods=["GET", "POST"])
def login():
    if current_user.is_authenticated: return redirect(url_for('index'))
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
                with conn.cursor() as cur:
                    cur.execute("UPDATE admins SET last_login=NOW() WHERE id=%s", (admin_data['id'],))
                return redirect(url_for('index'))
            else:
                flash('用户名或密码错误', 'danger')
        finally: conn.close()
    return render_template("login.html")

@app.route("/logout")
@login_required
def logout():
    logout_user()
    flash('您已安全退出系统', 'info')
    return redirect(url_for('login'))

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
                cur.execute("UPDATE users SET balance=%s, total_recharge=total_recharge+%s WHERE id=%s", (new_bal, amount, user_id))
                cur.execute("INSERT INTO recharge_log (user_id, amount, balance_after, remark, created_at) VALUES (%s, %s, %s, '管理员充值', NOW())", 
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
        with conn.cursor() as cur: cur.execute("DELETE FROM users WHERE id=%s", (user_id,))
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
                elif target_device: target_list = [target_device]
            for did in target_list: send_mqtt_cmd(did, "msg", message)
            return redirect(url_for("broadcast"))
        else:
            with conn.cursor() as cur:
                cur.execute("SELECT * FROM broadcast_log ORDER BY created_at DESC LIMIT 50")
                logs = cur.fetchall()
                # ★ 修复1：同时查询 device_id 和 seat_name
                cur.execute("SELECT device_id, seat_name FROM devices ORDER BY device_id ASC") 
                devices = cur.fetchall()
            return render_template("broadcast.html", logs=logs, devices=devices, admin_name=current_user.username)
    finally: conn.close()

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
        for dev in devices: send_mqtt_cmd(dev['device_id'], cmd_str)
        return jsonify({"status": "success", "message": "基础费率已更新 (会员刷卡会以此动态打折)"})
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
                 cur.execute("SELECT current_user_id FROM devices WHERE device_id=%s", (did,))
                 dev_row = cur.fetchone()
                 user_id = dev_row['current_user_id'] if dev_row else None

                 cur.execute("SELECT id, start_time FROM user_session_log WHERE device_id=%s AND end_time IS NULL ORDER BY id DESC LIMIT 1", (did,))
                 session_log = cur.fetchone()
                 
                 if session_log:
                     start_time = session_log['start_time']
                     if isinstance(start_time, str): start_time = datetime.strptime(start_time, "%Y-%m-%d %H:%M:%S")
                     now = datetime.now()
                     duration_sec = int((now - start_time).total_seconds())
                     
                     cur.execute("SELECT v FROM config WHERE k='price_per_min'")
                     row = cur.fetchone()
                     rate = float(row['v']) if row else 1.0

                     fee = 0.0
                     if user_id:
                         cur.execute("SELECT balance, total_recharge FROM users WHERE id=%s", (user_id,))
                         u_data = cur.fetchone()
                         if u_data:
                             total_rech = float(u_data['total_recharge'])
                             current_bal = float(u_data['balance'])
                             if total_rech >= 1000: rate *= 0.9
                             elif total_rech >= 500: rate *= 0.93
                             elif total_rech >= 300: rate *= 0.95
                             elif total_rech >= 100: rate *= 0.98

                             fee = round((duration_sec / 60.0) * rate, 2)
                             
                             if fee > current_bal:
                                 fee = current_bal

                     cur.execute("UPDATE user_session_log SET end_time=%s, duration_sec=%s, fee=%s, end_reason='admin_stop' WHERE id=%s", (now, duration_sec, fee, session_log['id']))
                     if user_id and fee > 0:
                         cur.execute("UPDATE users SET balance = balance - %s WHERE id=%s", (fee, user_id))
                         cur.execute("INSERT INTO consume_log (user_id, session_id, amount, created_at) VALUES (%s, %s, %s, NOW())", (user_id, session_log['id'], fee))

                 cur.execute("UPDATE devices SET current_status=0, current_user_id=NULL, current_sec=0, current_fee=0 WHERE device_id=%s", (did,))
        
        send_mqtt_cmd(did, cmd, val)
        return jsonify({"status": "ok"})
    except Exception as e: return jsonify({"status": "error", "message": str(e)}), 500
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
        # 发送一条带有特殊前缀的消息给单片机
        send_mqtt_cmd(did, "msg", f"SYS_RENAME:{new_name}")
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
            if mode == 'weekly': sql = "SELECT DATE_FORMAT(start_time, '%Y第%u周') as d, SUM(fee) as total_fee, COUNT(*) as cnt FROM user_session_log WHERE start_time >= DATE_SUB(CURDATE(), INTERVAL 12 WEEK) GROUP BY d ORDER BY d"
            elif mode == 'monthly': sql = "SELECT DATE_FORMAT(start_time, '%Y-%m') as d, SUM(fee) as total_fee, COUNT(*) as cnt FROM user_session_log WHERE start_time >= DATE_SUB(CURDATE(), INTERVAL 12 MONTH) GROUP BY d ORDER BY d"
            else: sql = "SELECT DATE(start_time) as d, SUM(fee) as total_fee, COUNT(*) as cnt FROM user_session_log WHERE start_time >= DATE_SUB(CURDATE(), INTERVAL 30 DAY) GROUP BY d ORDER BY d"
            cur.execute(sql)
            rows = cur.fetchall()
            return jsonify({"labels": [str(r['d']) for r in rows], "data_fee": [float(r['total_fee'] or 0) for r in rows], "data_cnt": [int(r['cnt'] or 0) for r in rows]})
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
            for i in range(duration_hours): hour_counts[(start_h + i) % 24] += 1
        return jsonify({"hours": [str(i) for i in range(24)], "avg_occupancy": [round(c / 30.0, 1) for c in hour_counts]})
    finally: conn.close()

if __name__ == "__main__":
    print("🚀 智能无人网吧 用户门户: http://localhost:5000/portal")
    print("🚀 智能无人网吧 管理后台: http://localhost:5000")
    app.run(host="0.0.0.0", port=5000, debug=True, use_reloader=False)