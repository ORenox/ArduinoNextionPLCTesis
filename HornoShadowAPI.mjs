import {
  IoTDataPlaneClient,
  GetThingShadowCommand,
  UpdateThingShadowCommand
} from "@aws-sdk/client-iot-data-plane";

// Configuración de Supabase
const SUPABASE_URL = process.env.SUPABASE_URL;
const SUPABASE_KEY = process.env.SUPABASE_ANON_KEY;

const headers = {
  "Content-Type": "application/json",
  "apikey": SUPABASE_KEY,
  "Authorization": `Bearer ${SUPABASE_KEY}`
};

// Cliente IoT
const client = new IoTDataPlaneClient({
  region: "us-east-1",
  endpoint: "https://axl7m09zqye1i-ats.iot.us-east-1.amazonaws.com"
});

const thingName = "HornoConeccionAWS";

// Almacenamiento simple para valores previos
let previousValues = {};

// 📋 TODAS LAS SEÑALES A MONITOREAR
const SIGNALS_TO_MONITOR = [
  // Digitales (para reglas)
  "M..1:22-1",
  "M..1:23-1",
  "Q..1:10-1",
  "Q..1:8-1",
  "Q..1:9-1",
  "Q..1:12-1",
  "I..1:5-1",
  // Analógicas (solo guardar)
  "AI..4:3-1",
  "AI..4:1-1"
];

// Labels descriptivos
const SIGNAL_LABELS = {
  "M..1:22-1": "Auto Vulcanizadora",
  "M..1:23-1": "Auto Centrifugo",
  "Q..1:10-1": "Pistón",
  "Q..1:8-1": "Motor centrifugo",
  "Q..1:9-1": "Resistencias",
  "Q..1:12-1": "Motor vulcanizadora",
  "I..1:5-1": "Emergencia",
  "AI..4:3-1": "Presión vulcanizadora",
  "AI..4:1-1": "Temperatura vulcanizadora"
};

export const handler = async (event) => {
  try {
    const method = event.requestContext.http.method;
    const path = event.requestContext.http.path;

    if (method === "GET" && path === "/shadow") {
      const resp = await client.send(new GetThingShadowCommand({ thingName }));
      const shadow = JSON.parse(Buffer.from(resp.payload).toString());

      return {
        statusCode: 200,
        headers: { "Access-Control-Allow-Origin": "*" },
        body: JSON.stringify({
          ...shadow.state.reported,
          ...shadow.state.desired
        })
      };
    }

    if (method === "POST" && path === "/shadow") {
      const { attribute, value } = JSON.parse(event.body);

      const payload = {
        state: { desired: { [attribute]: value } }
      };

      await client.send(new UpdateThingShadowCommand({
        thingName,
        payload: Buffer.from(JSON.stringify(payload))
      }));

      return {
        statusCode: 200,
        headers: { "Access-Control-Allow-Origin": "*" },
        body: JSON.stringify({ ok: true })
      };
    }

    if (method === "POST" && path === "/procesar") {
      await procesarCambiosDePLC();
      return {
        statusCode: 200,
        headers: { "Access-Control-Allow-Origin": "*" },
        body: JSON.stringify({ ok: true })
      };
    }

    return { 
      statusCode: 404, 
      headers: { "Access-Control-Allow-Origin": "*" },
      body: JSON.stringify({ error: "Not found" }) 
    };

  } catch (err) {
    console.error(err);
    return { 
      statusCode: 500, 
      headers: { "Access-Control-Allow-Origin": "*" },
      body: JSON.stringify({ error: err.message }) 
    };
  }
};

async function procesarCambiosDePLC() {
  try {
    const resp = await client.send(new GetThingShadowCommand({ thingName }));
    const shadow = JSON.parse(Buffer.from(resp.payload).toString());
    const reported = shadow.state.reported;

    for (const signal of SIGNALS_TO_MONITOR) {
      const current = reported[signal];
      if (current === undefined) continue;

      const previous = await getPrevious(signal);
      const label = SIGNAL_LABELS[signal] || signal;

      // 📊 ANALÓGICAS: Guardar lectura directamente
      if (signal === "AI..4:3-1" || signal === "AI..4:1-1") {
        await guardarEnTablaUnica({
          tipo: "lectura",
          signal_id: signal,
          sensor: label,
          valor: parseFloat(current),
          unidad: signal === "AI..4:3-1" ? "bar" : "°C",
          timestamp: new Date().toISOString()
        });
      }
      
      // 📊 DIGITALES: Evaluar reglas y guardar eventos
      if (previous !== null && previous !== current) {
        await evaluarReglas(signal, previous, current, reported);
      }

      await setPrevious(signal, current);
    }
    
  } catch (error) {
    console.error("Error en procesarCambiosDePLC:", error);
    throw error;
  }
}

// 📋 UNA SOLA FUNCIÓN PARA GUARDAR TODO
async function guardarEnTablaUnica(datos) {
  try {
    if (!SUPABASE_URL || !SUPABASE_KEY) {
      console.error("Faltan variables de entorno de Supabase");
      return;
    }

    const response = await fetch(`${SUPABASE_URL}/rest/v1/eventos_industriales`, {
      method: "POST",
      headers,
      body: JSON.stringify(datos)
    });

    if (!response.ok) {
      throw new Error(`Error guardando: ${response.statusText}`);
    }

    console.log(`💾 Guardado: ${datos.tipo} - ${datos.signal_id || datos.maquina}`);
  } catch (error) {
    console.error("Error guardando en tabla única:", error);
  }
}

// 📋 TUS REGLAS ORIGINALES - MODIFICADAS PARA USAR TABLA ÚNICA
async function evaluarReglas(signal, prev, curr, reported) {
  const isRise = (prev, curr) => prev === "00" && curr === "01";
  const isFall = (prev, curr) => prev === "01" && curr === "00";

  const m22 = reported["M..1:22-1"];
  const m23 = reported["M..1:23-1"];

  if (signal === "M..1:22-1") {
    if (isRise(prev, curr)) {
      await guardarEnTablaUnica({
        tipo: "evento",
        maquina: "vulcanizadora",
        modo_operacion: "automático",
        comentario: "Vulcanizadora activada en modo automático",
        signal_id: signal,
        timestamp: new Date().toISOString()
      });
    }
    if (isFall(prev, curr)) {
      await guardarEnTablaUnica({
        tipo: "evento",
        maquina: "vulcanizadora",
        modo_operacion: "automático",
        comentario: "Modo automático finalizado de la vulcanizadora",
        signal_id: signal,
        timestamp: new Date().toISOString()
      });
    }
  }

  if (signal === "M..1:23-1") {
    if (isRise(prev, curr)) {
      await guardarEnTablaUnica({
        tipo: "evento",
        maquina: "Horno centrifugo",
        modo_operacion: "automático",
        comentario: "Horno centrifugo activado en modo automático",
        signal_id: signal,
        timestamp: new Date().toISOString()
      });
    }
    if (isFall(prev, curr)) {
      await guardarEnTablaUnica({
        tipo: "evento",
        maquina: "Horno centrifugo",
        modo_operacion: "automático",
        comentario: "Modo automático finalizado del Horno centrifugo",
        signal_id: signal,
        timestamp: new Date().toISOString()
      });
    }
  }

  if (isRise(prev, curr) && m23 === "00") {
    if (signal === "Q..1:10-1") {
      await guardarEnTablaUnica({
        tipo: "evento",
        maquina: "Horno centrifugo",
        modo_operacion: "manual",
        comentario: "Pistón activado",
        signal_id: signal,
        timestamp: new Date().toISOString()
      });
    }
    if (signal === "Q..1:8-1") {
      await guardarEnTablaUnica({
        tipo: "evento",
        maquina: "Horno centrifugo",
        modo_operacion: "manual",
        comentario: "Motor del horno centrifugo activado",
        signal_id: signal,
        timestamp: new Date().toISOString()
      });
    }
  }

  if (isRise(prev, curr) && m22 === "00") {
    if (signal === "Q..1:9-1") {
      await guardarEnTablaUnica({
        tipo: "evento",
        maquina: "Vulcanizadora",
        modo_operacion: "manual",
        comentario: "Resistencias activadas",
        signal_id: signal,
        timestamp: new Date().toISOString()
      });
    }
    if (signal === "Q..1:12-1") {
      await guardarEnTablaUnica({
        tipo: "evento",
        maquina: "Vulcanizadora",
        modo_operacion: "manual",
        comentario: "Motor vulcanizadora activado",
        signal_id: signal,
        timestamp: new Date().toISOString()
      });
    }
  }

  if (signal === "I..1:5-1" && isFall(prev, curr)) {
    if (m22 === "01") {
      await guardarEnTablaUnica({
        tipo: "evento",
        maquina: "Vulcanizadora",
        modo_operacion: "automático",
        comentario: "Emergencia en modo automático vulcanizadora",
        signal_id: signal,
        timestamp: new Date().toISOString()
      });
    } else {
      await guardarEnTablaUnica({
        tipo: "evento",
        maquina: "Vulcanizadora",
        modo_operacion: "manual",
        comentario: "Emergencia en modo manual vulcanizadora",
        signal_id: signal,
        timestamp: new Date().toISOString()
      });
    }

    if (m23 === "01") {
      await guardarEnTablaUnica({
        tipo: "evento",
        maquina: "Horno centrifugo",
        modo_operacion: "automático",
        comentario: "Emergencia en modo automático centrifugadora",
        signal_id: signal,
        timestamp: new Date().toISOString()
      });
    } else {
      await guardarEnTablaUnica({
        tipo: "evento",
        maquina: "Horno centrifugo",
        modo_operacion: "manual",
        comentario: "Emergencia en modo manual centrifugadora",
        signal_id: signal,
        timestamp: new Date().toISOString()
      });
    }
  }
}

// Funciones auxiliares para valores previos
const getPrevious = async (signal) => {
  return previousValues[signal] || null;
};

const setPrevious = async (signal, value) => {
  previousValues[signal] = value;
};
