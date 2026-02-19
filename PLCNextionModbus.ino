#include <SPI.h>
#include <Ethernet.h>
#include <ArduinoModbus.h>
#include "max6675.h"
#include <HardwareSerial.h> 

// ============ CONFIGURACIÓN ============
SemaphoreHandle_t spiMutex;
SemaphoreHandle_t nextionMutex;

// Nextion
HardwareSerial nextion(2);

// MAX6675
int thermoSO = 27;
int thermoCS = 26;
int thermoSCK = 25;
MAX6675 thermocouple(thermoSCK, thermoCS, thermoSO);

// Ethernet W5500
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip(10, 1, 91, 98);
IPAddress plc_ip(10, 1, 91, 97);
EthernetClient ethClient;
ModbusTCPClient modbus(ethClient);

// Variables PLC
volatile bool entradasPLC[4] = {false, false, false, false};
volatile uint16_t analogPLC[8] = {0};
volatile bool marcasPLC[16] = {false};
volatile uint16_t amPLC[2] = {0};
volatile float temperatura = 0.0;

// Variables de control
static uint32_t ultimaLecturaTemp = 0;
static uint32_t ultimoReintentoPLC = 0;
static uint8_t fallosPLC = 0;
static bool modbusConectado = false;
static uint32_t ultimoHeartbeat = 0;

// Buffer para datos PLC
static uint8_t datosDiscretos[3];
static uint8_t datosCoils[2];
static uint16_t datosInput[8];
static uint16_t datosHolding[2];

// ============ PROTOTIPOS ============
bool conectarModbus();
void reconectarEthernet();
bool leerTodosLosRegistros();
void limpiarBufferModbus();
void actualizarNextionPagina(int pagina);
void sendToNextion(const char *cmd);

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Iniciando sistema...");
  
  // Crear mutexes
  nextionMutex = xSemaphoreCreateMutex();
  spiMutex = xSemaphoreCreateMutex();
  
  // Configurar Nextion
  nextion.begin(9600, SERIAL_8N1, 16, 17);
  
  // Configurar Ethernet
  Ethernet.init(5);
  ethClient.setConnectionTimeout(3000);
  ethClient.setTimeout(2000);
  
  if (xSemaphoreTake(spiMutex, portMAX_DELAY) == pdTRUE) {
    Ethernet.begin(mac, ip);
    xSemaphoreGive(spiMutex);
  }
  
  Serial.print("IP Local: ");
  Serial.println(Ethernet.localIP());
  
  vTaskDelay(pdMS_TO_TICKS(2000));
  
  // Crear tasks con prioridades ajustadas
  xTaskCreatePinnedToCore(TaskPLC, "PLC", 6000, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskTemp, "TEMP", 3000, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskNextion, "NEXTION", 8000, NULL, 1, NULL, 0);
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}

// ============ FUNCIONES AUXILIARES ============
bool conectarModbus() {
  if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
    if (!modbusConectado) {
      if (ethClient.connected()) {
        ethClient.stop();
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      
      Serial.println("Conectando Modbus...");
      modbusConectado = modbus.begin(plc_ip, 510);
      
      if (modbusConectado) {
        Serial.println("Modbus conectado");
        fallosPLC = 0;
        ethClient.setTimeout(2000);
      } else {
        Serial.println("Error conectando Modbus");
      }
    }
    xSemaphoreGive(spiMutex);
  }
  return modbusConectado;
}

void reconectarEthernet() {
  Serial.println("Reiniciando Ethernet...");
  
  if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
    if (ethClient.connected()) {
      ethClient.stop();
    }
    modbus.end();
    modbusConectado = false;
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    Ethernet.begin(mac, ip);
    ethClient.setConnectionTimeout(3000);
    ethClient.setTimeout(2000);
    
    Serial.print("Ethernet reiniciado. IP: ");
    Serial.println(Ethernet.localIP());
    
    xSemaphoreGive(spiMutex);
  }
  
  vTaskDelay(pdMS_TO_TICKS(2000));
  conectarModbus();
}

bool leerTodosLosRegistros() {
  if (!modbusConectado || !modbus.connected()) {
    return false;
  }
  
  bool exito = true;
  
  // Leer Discrete Inputs (I)
  if (!modbus.requestFrom(1, DISCRETE_INPUTS, 0, 3)) {
    exito = false;
  } else {
    for (int i = 0; i < 3 && modbus.available(); i++) {
      entradasPLC[i] = modbus.read();
    }
    limpiarBufferModbus();
  }
  
  vTaskDelay(pdMS_TO_TICKS(50));
  
  // Leer Coils (M)
  if (exito && !modbus.requestFrom(1, COILS, 8192, 16)) {
    exito = false;
  } else {
    for (int i = 0; i < 16 && modbus.available(); i++) {
      marcasPLC[i] = modbus.read();
    }
    limpiarBufferModbus();
  }
  
  vTaskDelay(pdMS_TO_TICKS(50));
  
  // Leer Input Registers (AI)
  if (exito && !modbus.requestFrom(1, INPUT_REGISTERS, 0, 8)) {
    exito = false;
  } else {
    for (int i = 0; i < 8 && modbus.available(); i++) {
      analogPLC[i] = modbus.read();
    }
    limpiarBufferModbus();
  }
  
  vTaskDelay(pdMS_TO_TICKS(50));
  
  // Leer Holding Registers (AM)
  if (exito && !modbus.requestFrom(1, HOLDING_REGISTERS, 528, 2)) {
    exito = false;
  } else {
    for (int i = 0; i < 2 && modbus.available(); i++) {
      amPLC[i] = modbus.read();
    }
    limpiarBufferModbus();
  }
  
  return exito;
}

void limpiarBufferModbus() {
  uint32_t timeout = millis() + 100;
  while (modbus.available() && millis() < timeout) {
    modbus.read();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// ============ TASK PLC OPTIMIZADA ============
void TaskPLC(void *pvParameters) {
  uint32_t ultimaLecturaCompleta = 0;
  const uint32_t INTERVALO_LECTURA = 1000; // Leer todo cada 1 segundo
  
  for (;;) {
    uint32_t ahora = millis();
    
    // Verificar conexión
    if (modbusConectado && !modbus.connected()) {
      Serial.println("PLC desconectado");
      modbusConectado = false;
    }
    
    // Reconectar si es necesario
    if (!modbusConectado) {
      if (ahora - ultimoReintentoPLC > 5000) {
        ultimoReintentoPLC = ahora;
        conectarModbus();
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    
    // Leer datos completos a intervalos
    if (ahora - ultimaLecturaCompleta >= INTERVALO_LECTURA) {
      
      if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        
        bool ok = leerTodosLosRegistros();
        
        xSemaphoreGive(spiMutex);
        
        if (!ok) {
          fallosPLC++;
          Serial.printf("Fallo %d en lectura PLC\n", fallosPLC);
          
          if (fallosPLC >= 3) {
            Serial.println("Demasiados fallos, reiniciando conexión");
            modbusConectado = false;
            fallosPLC = 0;
          }
        } else {
          if (fallosPLC > 0) {
            Serial.println("Lectura PLC OK");
            fallosPLC = 0;
          }
          ultimaLecturaCompleta = ahora;
        }
      }
    }

    if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {

      if (modbusConectado && modbus.connected()) {
        
        uint16_t tempWord = (uint16_t)(temperatura * 10);

        bool ok = modbus.holdingRegisterWrite(23, tempWord);

        if (!ok) {
          Serial.println("Error escribiendo VW44");
        } else {
          Serial.println("VW44 actualizado");
        }
      }

      xSemaphoreGive(spiMutex);
    }
    
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ============ TASK TEMPERATURA ============
void TaskTemp(void *pvParameters) {
  const uint32_t INTERVALO_TEMP = 2000; // Leer temperatura cada 2 segundos
  float tempBuffer = 0;
  int lecturasValidas = 0;
  
  for (;;) {
    uint32_t ahora = millis();
    
    if (ahora - ultimaLecturaTemp >= INTERVALO_TEMP) {
      
      if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        float nuevaTemp = thermocouple.readCelsius();
        
        if (nuevaTemp > -50 && nuevaTemp < 200) {
          tempBuffer += nuevaTemp;
          lecturasValidas++;
          
          if (lecturasValidas >= 3) {
            temperatura = tempBuffer / lecturasValidas;
            tempBuffer = 0;
            lecturasValidas = 0;
          }
        }
        
        xSemaphoreGive(spiMutex);
        ultimaLecturaTemp = ahora;
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ============ TASK NEXTION ============
void TaskNextion(void *pvParameters) {
  int paginaActual = -1;
  uint32_t ultimaActualizacion = 0;
  const uint32_t INTERVALO_NEXTION = 800; // Actualizar cada 800ms
  
  for (;;) {
    uint32_t ahora = millis();
    
    if (ahora - ultimaActualizacion >= INTERVALO_NEXTION) {
      
      // Decidir página
      int selector = (entradasPLC[0] ? 2 : 0) | (entradasPLC[1] ? 1 : 0);
      int paginaNueva;
      
      switch (selector) {
        case 0: paginaNueva = 0; break; // Centrifugadora Auto
        case 1: paginaNueva = 2; break; // Centrifugadora Manual
        case 2: paginaNueva = 1; break; // Vulcanizadora Auto
        case 3: paginaNueva = 3; break; // Vulcanizadora Manual
        default: paginaNueva = 0; break;
      }
      
      // Cambiar página si es necesario
      if (paginaNueva != paginaActual) {
        char cmd[30];
        snprintf(cmd, sizeof(cmd), "page %d", paginaNueva);
        sendToNextion(cmd);
        paginaActual = paginaNueva;
        vTaskDelay(pdMS_TO_TICKS(200));
      }
      
      // Actualizar datos
      actualizarNextionPagina(paginaActual);
      ultimaActualizacion = ahora;
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void actualizarNextionPagina(int pagina) {
  char cmd[80];
  
  switch (pagina) {
    case 0: { // Centrifugadora Automático
      snprintf(cmd, sizeof(cmd), "t0.txt=\"%d\"", (int)temperatura);
      sendToNextion(cmd);
      
      sendToNextion("t1.txt=\"AUTO\"");
      
      snprintf(cmd, sizeof(cmd), "t2.txt=\"%d\"", amPLC[1]);
      sendToNextion(cmd);
      
      snprintf(cmd, sizeof(cmd), "t3.txt=\"%s\"", marcasPLC[7] ? "ON" : "OFF");
      sendToNextion(cmd);
      
      snprintf(cmd, sizeof(cmd), "t4.txt=\"%s\"", marcasPLC[9] ? "ON" : "OFF");
      sendToNextion(cmd);
      break;
    }
      
    case 1: { // Vulcanizadora Automático
      int tempVulc = (int)(analogPLC[0] * 0.25 - 50);
      snprintf(cmd, sizeof(cmd), "t5.txt=\"%d\"", tempVulc);
      sendToNextion(cmd);
      
      int presion = (int)((analogPLC[2] - 200) * 3.64 + 10);
      snprintf(cmd, sizeof(cmd), "t6.txt=\"%d\"", presion);
      sendToNextion(cmd);
      
      snprintf(cmd, sizeof(cmd), "t7.txt=\"%d\"", amPLC[0]);
      sendToNextion(cmd);
      
      snprintf(cmd, sizeof(cmd), "t8.txt=\"%s\"", marcasPLC[8] ? "ON" : "OFF");
      sendToNextion(cmd);
      
      snprintf(cmd, sizeof(cmd), "t9.txt=\"%s\"", marcasPLC[11] ? "ON" : "OFF");
      sendToNextion(cmd);
      break;
    }
      
    case 2: { // Centrifugadora Manual
      snprintf(cmd, sizeof(cmd), "t10.txt=\"%d\"", (int)temperatura);
      sendToNextion(cmd);
      
      sendToNextion("t11.txt=\"MANUAL\"");
      
      snprintf(cmd, sizeof(cmd), "t12.txt=\"%s\"", marcasPLC[7] ? "ON" : "OFF");
      sendToNextion(cmd);
      
      snprintf(cmd, sizeof(cmd), "t13.txt=\"%s\"", marcasPLC[9] ? "ON" : "OFF");
      sendToNextion(cmd);
      break;
    }
      
    case 3: { // Vulcanizadora Manual
      int tempVulc = (int)(analogPLC[0] * 0.25 - 50);
      snprintf(cmd, sizeof(cmd), "t14.txt=\"%d\"", tempVulc);
      sendToNextion(cmd);
      
      int presion = (int)((analogPLC[2] - 200) * 3.64 + 10);
      snprintf(cmd, sizeof(cmd), "t15.txt=\"%d\"", presion);
      sendToNextion(cmd);
      
      snprintf(cmd, sizeof(cmd), "t16.txt=\"%s\"", marcasPLC[8] ? "ON" : "OFF");
      sendToNextion(cmd);
      
      snprintf(cmd, sizeof(cmd), "t17.txt=\"%s\"", marcasPLC[11] ? "ON" : "OFF");
      sendToNextion(cmd);
      break;
    }
  }
}

// ============ SEND TO NEXTION ============
void sendToNextion(const char *cmd) {
  if (xSemaphoreTake(nextionMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    nextion.print(cmd);
    nextion.write(0xFF);
    nextion.write(0xFF);
    nextion.write(0xFF);
    
    // Pequeña pausa para que Nextion procese
    vTaskDelay(pdMS_TO_TICKS(20));
    
    xSemaphoreGive(nextionMutex);
  }
}