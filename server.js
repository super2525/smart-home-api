require('dotenv').config();
const express = require('express');
const bodyParser = require('body-parser');
const cors = require('cors');
const mongoose = require('mongoose');
const jwt = require('jsonwebtoken');
const bcrypt = require('bcrypt');
const path = require('path');

const app = express();

/* -------------------- 1. Config & Middleware -------------------- */
const corsOptions = {
    origin: '*',
    methods: ['GET','POST','DELETE'],
    allowedHeaders: ['Content-Type','Authorization']
};
app.use(cors(corsOptions));

// รองรับ Binary Data (สำหรับ ESP32)
app.use(bodyParser.raw({ type: "application/octet-stream", limit: "1kb" }));
// รองรับ JSON (สำหรับหน้าเว็บ)
app.use(bodyParser.json());

// ให้ Server ส่งไฟล์ Static (HTML, CSS, JS) ในโฟลเดอร์ public
app.use(express.static(path.join(__dirname, 'public')));

/* -------------------- 2. MongoDB Connection -------------------- */
mongoose.connect(process.env.MONGO_URI)
.then(() => console.log("MongoDB Connected"))
.catch(err => console.log("MongoDB Error", err));

/* -------------------- 3. Schemas -------------------- */

// User Schema (เก็บผู้ใช้งาน)
const userSchema = new mongoose.Schema({
    username: { type: String, unique: true, required: true },
    password: { type: String, required: true },
    role: { type: String, enum: ['admin','user'], default: 'user' }
});
const User = mongoose.model("User", userSchema, "SmartHomeUsers");

// State Schema (เก็บสถานะไฟแยกราย Device)
const stateSchema = new mongoose.Schema({
    _id: { type: String, default: "GLOBAL_STATE" }, // ใช้ deviceID เป็น _id
    bitmask: { type: Number, default: 0 }
});
const State = mongoose.model("State", stateSchema, "SmartHomeState");

// Schedule Schema (เก็บตารางเวลา)
const scheduleSchema = new mongoose.Schema({
    deviceID: { type: String, required: true },
    pinIndex: { type: Number, required: true },
    action: { type: String, enum: ['ON', 'OFF'], required: true },
    time: { type: String, required: true }, // Format "HH:mm"
    note: String,
    createdBy: { type: String } // เก็บชื่อคนสร้างรายการ
}, { timestamps: true }); // Auto created at/updated at

const Schedule = mongoose.model("Schedule", scheduleSchema, "SmartHomeSchedules");

/* -------------------- 4. Helper Functions -------------------- */
function verifyToken(req, res, next) {
    const auth = req.headers["authorization"];
    if(!auth) return res.status(401).json({ error: "Missing token" });

    const token = auth.split(" ")[1];
    if(!token) return res.status(401).json({ error: "Invalid header" });

    jwt.verify(token, process.env.JWT_SECRET, (err, user) => {
        if(err) return res.status(403).json({ error: "Invalid token" });
        req.user = user; // { id, username, role }
        next();
    });
}

/* -------------------- 5. API: Authentication -------------------- */
app.post('/api/login', async (req, res) => {
    const { username, password } = req.body;

    // Auto-create admin (ถ้ายังไม่มี)
    let admin = await User.findOne({ username: "admin" });
    if(!admin) {
        const hashed = await bcrypt.hash("22556677", 10);
        admin = await User.create({ username: "admin", password: hashed, role: "admin" });
        console.log("Admin auto-created");
    }

    const user = await User.findOne({ username });
    if(!user) return res.status(401).json({ error: "User not found" });

    const match = await bcrypt.compare(password, user.password);
    if(!match) return res.status(401).json({ error: "Invalid password" });

    // สร้าง Token (อายุ 2 ชม.)
    const token = jwt.sign(
        { id: user._id, username: user.username, role: user.role },
        process.env.JWT_SECRET,
        { expiresIn: "2h" }
    );

    res.json({ token });
});

app.post("/api/create-user", verifyToken, async (req,res)=>{
    if(req.user.role !== "admin") return res.status(403).json({ error: "Not allowed" });

    const { username, password } = req.body;
    try {
        const hashed = await bcrypt.hash(password, 10);
        await User.create({ username, password: hashed, role: "user" });
        res.json({ success: true, message: "User created" });
    } catch (e) {
        res.status(400).json({ error: "Username likely exists" });
    }
});

/* -------------------- 6. API: Device Control (Binary Protocol) -------------------- */

// ESP32 ส่งค่ามา Update (หรือหน้าเว็บกดสั่ง)
app.post("/api/device/:id/setState", verifyToken, async (req, res) => {
    try {
        const deviceID = req.params.id;
        const raw = req.body;

        if (!Buffer.isBuffer(raw) || raw.length < 2) {
            return res.status(400).json({ error: "Must send 2-byte binary" });
        }

        const newMask = raw.readUInt16BE(0);

        // Update ลง DB โดยตรง
        await State.updateOne(
            { _id: deviceID },
            { $set: { bitmask: newMask } },
            { upsert: true }
        );
        
        res.json({ success: true });
    } catch (err) {
        console.error("[setState] Error:", err);
        res.status(500).json({ error: err.message });
    }
});

// ESP32 Poll ค่า (หรือหน้าเว็บดึงสถานะ)
app.get("/api/device/:id/getState", verifyToken, async (req, res) => {
    try {
        const deviceID = req.params.id;
        
        // อ่านจาก DB โดยตรง
        let state = await State.findById(deviceID);
        
        // ถ้ายังไม่มีข้อมูล ให้สร้างใหม่ (Init)
        if (!state) {
            state = await State.create({ _id: deviceID, bitmask: 0 });
            console.log(`New device registered: ${deviceID}`);
        } 

        // ส่งกลับเป็น Binary 2 Bytes
        res.setHeader("Content-Type", "application/octet-stream");
        const buf = Buffer.alloc(2);
        buf.writeUInt16BE(state.bitmask, 0);

        res.end(buf);
    } catch (err) {
        console.error("Error fetching bitmask:", err.message);
        res.status(500).json({ error: err.message });
    }
});

/* -------------------- 7. API: Schedule Management -------------------- */

// เพิ่มตารางเวลา (Admin Only)
app.post("/api/schedule", verifyToken, async (req, res) => {
    try {
        if (req.user.role !== 'admin') {
            return res.status(403).json({ error: "Admin only" });
        }

        const { deviceID, pinIndex, action, time } = req.body;
        
        await Schedule.create({ 
            deviceID, 
            pinIndex, 
            action, 
            time,
            createdBy: req.user.username // บันทึกชื่อคนทำรายการ
        });

        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// ดูรายการทั้งหมด (Everyone)
app.get("/api/schedule", verifyToken, async (req, res) => {
    try {
        const list = await Schedule.find().sort({ time: 1 });
        res.json(list);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// ลบรายการ (Admin Only)
app.delete("/api/schedule/:id", verifyToken, async (req, res) => {
    try {
        if (req.user.role !== 'admin') {
            return res.status(403).json({ error: "Admin only" });
        }

        await Schedule.findByIdAndDelete(req.params.id);
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

/* -------------------- 8. Scheduler Engine (Clock) -------------------- */
// ทำงานทุก 60 วินาที เพื่อเช็คเวลาและสั่งงาน
setInterval(async () => {
    try {
        const now = new Date();
        // ดึงเวลาไทยแบบ HH:mm
        const currentHHMM = now.toLocaleTimeString('en-GB', {
            timeZone: 'Asia/Bangkok',
            hour: '2-digit',
            minute: '2-digit',
            hour12: false
        });

        // 1. ค้นหาคำสั่งที่เวลาตรงกับปัจจุบัน
        const tasks = await Schedule.find({ time: currentHHMM });

        if (tasks.length > 0) {
            console.log(`[Scheduler] Time ${currentHHMM} matched ${tasks.length} tasks.`);
            
            // 2. วนลูปทำทีละคำสั่ง
            for (const task of tasks) {
                const targetDeviceID = task.deviceID; 

                // ดึง State ปัจจุบันจาก DB
                let state = await State.findById(targetDeviceID);
                
                if (!state) {
                    state = await State.create({ _id: targetDeviceID, bitmask: 0 });
                }

                let currentMask = state.bitmask;
                const pinMask = 1 << task.pinIndex;
                let isChanged = false;

                // คำนวณ Bitmask ใหม่
                if (task.action.toUpperCase() === 'ON') {
                    if ((currentMask & pinMask) === 0) { // ถ้ายังไม่เปิด
                        currentMask |= pinMask;
                        isChanged = true;
                    }
                } else if (task.action.toUpperCase() === 'OFF') {
                    if ((currentMask & pinMask) !== 0) { // ถ้ายังไม่ปิด
                        currentMask &= ~pinMask;
                        isChanged = true;
                    }
                }

                // 3. บันทึกผลลง DB (ถ้ามีการเปลี่ยนแปลง)
                if (isChanged) {
                    await State.updateOne(
                        { _id: targetDeviceID },
                        { $set: { bitmask: currentMask } }
                    ); 
                    console.log(`[Scheduler] Updated ${targetDeviceID}: Pin ${task.pinIndex} -> ${task.action}`);
                }
            }
        }
    } catch (err) {
        console.error("[Scheduler Error]", err);
    }
}, 60000); // Check every 60 seconds

/* -------------------- Start Server -------------------- */
const PORT = process.env.PORT || 3000;
app.listen(PORT, ()=>console.log(`Server running on port ${PORT}`));