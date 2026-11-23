"""test_ftp.py -- ftpd.c (built as ./server on a PC) against Python's ftplib.

Run: python3 test_ftp.py   (starts ./server on port 2121 over card.img)
"""
import ftplib, io, os, random, socket, subprocess, sys, time

fails = 0
def check(ok, what):
    global fails
    print(("ok: " if ok else "FAIL: ") + what)
    if not ok: fails += 1

srv = subprocess.Popen(["./server"], stdout=subprocess.PIPE, text=True)
assert srv.stdout.readline().strip() == "listening"
try:
    try:
        f = ftplib.FTP(); f.connect("127.0.0.1", 2121, timeout=10); f.login("chris", "wrong")
        check(False, "a wrong password is refused")
    except ftplib.error_perm as e:
        check(str(e).startswith("530"), "a wrong password is refused")
    f.close(); time.sleep(0.2)
    f = ftplib.FTP(); f.connect("127.0.0.1", 2121, timeout=10)
    check(f.getwelcome().startswith("220"), "welcome")
    try:
        f.nlst(); check(False, "nothing before logging in")
    except ftplib.error_perm as e:
        check(str(e).startswith("530"), "nothing before logging in")
    f.login("chris", "calc")
    check(f.pwd() == "/", "starts at /")
    names = f.nlst()
    check("readme.txt" in names and "docs" in names, "NLST: %s" % names)
    lines = []; f.retrlines("LIST", lines.append)
    check(any(l.startswith("d") and l.endswith(" docs") for l in lines) and any("readme.txt" in l and " 11 " in l for l in lines), "LIST in ls -l form")
    b = io.BytesIO(); f.retrbinary("RETR readme.txt", b.write)
    check(b.getvalue() == b"hello card\n", "RETR a small file")
    check(f.size("readme.txt") == 11, "SIZE")
    check(f.sendcmd("MDTM readme.txt").startswith("213 20"), "MDTM")
    data = bytes(random.Random(1).getrandbits(8) for _ in range(300_000))
    f.cwd("docs"); check(f.pwd() == "/docs", "CWD into a folder")
    f.storbinary("STOR big.bin", io.BytesIO(data))
    b = io.BytesIO(); f.retrbinary("RETR big.bin", b.write)
    check(b.getvalue() == data, "300 KB up and back, intact")
    f.storbinary("APPE big.bin", io.BytesIO(b"tail")); check(f.size("big.bin") == len(data) + 4, "APPE")
    f.cwd(".."); check(f.pwd() == "/", "CWD ..")
    f.sendcmd("CWD /docs/../docs/./"); check(f.pwd() == "/docs", "paths with . and ..")
    f.sendcmd("CDUP"); check(f.pwd() == "/", "CDUP")
    f.mkd("new folder"); check("new folder" in f.nlst(), "MKD, with a space in the name")
    f.storbinary("STOR /new folder/a b.txt", io.BytesIO(b"x")); check(f.nlst("/new folder") == ["a b.txt"], "absolute paths, spaces")
    f.rename("/new folder/a b.txt", "/new folder/c.txt"); check(f.nlst("/new folder") == ["c.txt"], "RNFR/RNTO")
    try: f.rmd("new folder"); check(False, "RMD refuses a folder with something in it")
    except ftplib.error_perm: check(True, "RMD refuses a folder with something in it")
    f.delete("/new folder/c.txt"); f.rmd("new folder"); check("new folder" not in f.nlst(), "DELE and RMD")
    try: f.retrbinary("RETR nothing.txt", lambda d: None); check(False, "RETR of a missing file: 550")
    except ftplib.error_perm as e: check(str(e).startswith("550"), "RETR of a missing file: 550")
    try: f.cwd("/nowhere"); check(False, "CWD to a missing folder: 550")
    except ftplib.error_perm as e: check(str(e).startswith("550"), "CWD to a missing folder: 550")
    f.set_pasv(False)   # active mode: the server connects back (PORT)
    b = io.BytesIO(); f.retrbinary("RETR /readme.txt", b.write); check(b.getvalue() == b"hello card\n", "active mode (PORT)")
    f.set_pasv(True)
    check(f.sendcmd("EPSV").startswith("229"), "EPSV")
    # a second client while the first is connected
    g = socket.create_connection(("127.0.0.1", 2121), timeout=5)
    check(g.recv(100).startswith(b"421"), "a second client is told to come back later"); g.close()
    check(f.sendcmd("FEAT").startswith("211"), "FEAT")
    try: f.sendcmd("SITE CHMOD 777 x"); check(False, "unknown commands: 502")
    except ftplib.error_perm as e: check(str(e).startswith("502"), "unknown commands: 502")
    f.quit()
    # and after QUIT, a new client gets in
    h = ftplib.FTP(); h.connect("127.0.0.1", 2121, timeout=10); h.login("chris", "calc")
    check("docs" in h.nlst(), "the next client after QUIT")
    h.close()   # dropped without QUIT
    time.sleep(0.2)
    k = ftplib.FTP(); k.connect("127.0.0.1", 2121, timeout=10); k.login("chris", "calc")
    check(k.pwd() == "/", "the next client after one that just dropped"); k.quit()
finally:
    srv.kill()
print("%d failed" % fails); sys.exit(1 if fails else 0)
