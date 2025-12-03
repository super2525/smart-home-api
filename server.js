const express = require('express');
const bodyParser = require('body-parser');
const cors = require('cors');
const app = express();

app.use(cors());
app.use(bodyParser.json());

const deviceState = {}; // เก็บ state ของ ESP32

// Middleware ตรวจ API Key
app.use((req, res, next) => {
    const key = req.headers['authorization'];
    if(!key || key !== `Bearer mysecret123`) {
        return res.status(401).json({ error: 'Unauthorized' });
    }
    next();
});

// POST /device/:id/state
app.post('/device/:id/state', (req, res) => {
    const { id } = req.params;
    const { pin, state } = req.body;
    if(!deviceState[id]) deviceState[id] = {};
    deviceState[id][pin] = state;
    console.log(`Device ${id} Pin ${pin} = ${state}`);
    res.json({ success: true });
});

// GET /device/:id/control
app.get('/device/:id/control', (req, res) => {
    const { id } = req.params;
    res.json(deviceState[id] || {});
});

const PORT = process.env.PORT || 3000;
app.listen(PORT, () => console.log(`Server running on port ${PORT}`));
