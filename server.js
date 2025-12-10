require('dotenv').config();
const express = require('express');
const bodyParser = require('body-parser');
const cors = require('cors');
const mongoose = require('mongoose');
const jwt = require('jsonwebtoken');
const bcrypt = require('bcrypt');
const path = require('path');

const app = express();

/* -------------------- CORS -------------------- */
const corsOptions = {
    origin: '*',
    methods: ['GET','POST'],
    allowedHeaders: ['Content-Type','Authorization']
};
app.use(cors(corsOptions));
app.use(bodyParser.raw({ type: "application/octet-stream", limit: "1kb" }));
app.use(bodyParser.json());
app.use(express.static(path.join(__dirname, 'public')));

/* -------------------- MongoDB -------------------- */
mongoose.connect(process.env.MONGO_URI)
.then(() => console.log("MongoDB Connected"))
.catch(err => console.log("MongoDB Error", err));

/* -------------------- User Schema -------------------- */
const userSchema = new mongoose.Schema({
    username: { type: String, unique: true, required: true },
    password: { type: String, required: true },
    role: { type: String, enum: ['admin','user'], default: 'user' }
});
const User = mongoose.model("User", userSchema, "SmartHomeUsers");

/* -------------------- Smart Home State (bitmask) -------------------- */
const stateSchema = new mongoose.Schema({
    _id: { type: String, default: "GLOBAL_STATE" },
    bitmask: { type: Number, default: 0 }
});
const State = mongoose.model("State", stateSchema, "SmartHomeState");


/* -------------------- JWT Middleware -------------------- */
function verifyToken(req, res, next) {
    const auth = req.headers["authorization"];
    if(!auth) return res.status(401).json({ error: "Missing token" });

    const token = auth.split(" ")[1];
    if(!token) return res.status(401).json({ error: "Invalid header" });

    jwt.verify(token, process.env.JWT_SECRET, (err, user) => {
        if(err) return res.status(403).json({ error: "Invalid token" });
        req.user = user;
        next();
    });
}

/* -------------------- Login -------------------- */
app.post('/api/login', async (req, res) => {
    const { username, password } = req.body;

    // Auto-create admin if missing
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

    const token = jwt.sign(
        { id: user._id, username: user.username, role: user.role },
        process.env.JWT_SECRET,
        { expiresIn: "2h" }
    );

    res.json({ token });
});

/* -------------------- Create User (Admin Only) -------------------- */
app.post("/api/create-user", verifyToken, async (req,res)=>{
    if(req.user.role !== "admin")
        return res.status(403).json({ error: "Not allowed" });

    const { username, password } = req.body;

    const hashed = await bcrypt.hash(password, 10);
    await User.create({ username, password: hashed, role: "user" });

    res.json({ success: true, message: "User created" });
});

/* -------------------- Set Bitmask (RAW Binary) -------------------- */
/* -------------------- Set Bitmask -------------------- */
app.post("/api/device/:id/setState", verifyToken, async (req, res) => {
    try {
        const deviceID = req.params.id;
        const raw = req.body;

        if (!Buffer.isBuffer(raw) || raw.length < 2) {
            console.log("[setState] Error: Invalid Body (Not 2-byte Buffer)"); // <--- Log Error
            return res.status(400).json({ error: "Must send 2-byte binary" });
        }

        const newMask = raw.readUInt16BE(0);

        const result = await State.updateOne(
            { _id: deviceID },
            { $set: { bitmask: newMask } },
            { upsert: true }
        );
        
        res.json({ success: true });
    } catch (err) {
        console.error("[setState] Server Error:", err);
        res.status(500).json({ error: err.message });
    }
});

/* -------------------- Get Bitmask (RAW Binary) -------------------- */
app.get("/api/device/:id/getState", verifyToken, async (req, res) => {
    try {

        let state = await State.findById(req.params.id);
        if (!state) {
            //upsert default state
            state = await State.create({ _id: req.params.id, bitmask: 0 });
            console.log(`New device registered: ${req.params.id}`);
        } 

        res.setHeader("Content-Type", "application/octet-stream");
        const buf = Buffer.alloc(2);
        buf.writeUInt16BE(state.bitmask, 0);

        res.end(buf);
    } catch (err) {
        console.error("Error fetching bitmask:", err.message);
        res.status(500).json({ error: "Server error"+err.message });
    }
});

/* -------------------- Start Server ------------------*/
const PORT = process.env.PORT || 3000;
app.listen(PORT, ()=>console.log(`Server running on port ${PORT}`));