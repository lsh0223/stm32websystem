import pymysql
from werkzeug.security import generate_password_hash

# 1. 连接你的数据库
db = pymysql.connect(
    host="127.0.0.1", 
    port=3306, 
    user="root", 
    password="123456", 
    database="netbar", 
    autocommit=True
)
cursor = db.cursor()

# 2. 用当前服务器的环境生成 123456 的安全哈希密文
hashed_pwd = generate_password_hash('123456')

# 3. 清理可能存在的旧账号并插入新账号
cursor.execute("DELETE FROM admins WHERE username='admin'")
cursor.execute("INSERT INTO admins (username, password_hash) VALUES ('admin', %s)", (hashed_pwd,))

print("✅ 管理员账号已成功初始化/重置！账号: admin | 密码: 123456")
db.close()
