/* =============================================================================
   GUERREIROS DE REALENGO  ·  firmware de mini sumo  ·  InovaWeek UVV
   -----------------------------------------------------------------------------
   Placa   : ESP32 DevKit V1, 30 pinos   (Arduino core "esp32" 3.3.x)
   Ponte H : TB6612FNG
   Sensores: HC-SR04 (acha o adversario)  +  2x TCRT5000 (borda branca)

   ROBO 100% AUTONOMO: nao recebe comando nenhum.
     liga a chave -> espera 5 s (e calibra os sensores de linha)
     -> gira procurando o adversario -> ataca em linha reta
     -> se um sensor ver a borda branca, recua e gira de volta para o centro.
   Para parar, desligue a chave.

   Nenhuma biblioteca extra: so o pacote de placas "esp32" da Espressif.

   ESQUERDA e DIREITA sao sempre do ponto de vista do ROBO:
   robo na mesa, frente (o sonar) apontando para longe de voce.

   SENSOR DE LINHA: o codigo serve para as duas opcoes, sem mudar nada.
     A) TCRT5000 avulso + resistores de 100 ohm e 10 k  (opcao principal)
     B) modulo TCRT5000 com saida D0                    (opcao alternativa)
   Nas duas, o BRANCO da borda puxa a leitura para BAIXO.

   ANTES DE RODAR:
     1. Ajuste o LM2596 para 5,00 V com multimetro. So depois ligue o ESP32.
     2. Grave com MODO_TESTE = true e abra o Monitor Serial a 115200.
     3. Ponha o robo no preto e depois na borda branca. Veja os numeros da linha.
        Preto tem que dar bem mais que branco (ex.: 3000 contra 300).
     4. Aproxime a mao da frente do robo: a distancia tem que cair.
     5. Com o robo NO AR, digite  e  (motor esquerdo) e  d  (motor direito)
        no Monitor Serial. Cada roda deve girar PARA A FRENTE.
        Se girar para tras, troque os dois fios daquele motor na TB6612.
     6. Coloque MODO_TESTE = false e regrave.
   ============================================================================= */

// ---------------------------------------------------------------- MODO TESTE
// true  = os motores so giram quando voce manda (e / d) pelo cabo USB
// false = modo de combate: liga, espera 5 s e luta sozinho
const bool MODO_TESTE = true;

// ------------------------------------------------------------------- PINAGEM
// TB6612FNG  (canal A = motor ESQUERDO, canal B = motor DIREITO)
const int PIN_AIN1 = 26;
const int PIN_AIN2 = 27;
const int PIN_PWMA = 25;
const int PIN_BIN1 = 32;
const int PIN_BIN2 = 33;
const int PIN_PWMB = 14;
const int PIN_STBY = 13;   // HIGH habilita a ponte H

// HC-SR04  (ECHO passa pelo divisor 1 k / 2 k: 5 V -> 3,3 V)
const int PIN_TRIG = 5;
const int PIN_ECHO = 18;

// TCRT5000  (34 e 35 sao entradas analogicas do ADC1, so-entrada)
const int PIN_LINHA_ESQ = 34;
const int PIN_LINHA_DIR = 35;

// LED azul da placa
const int PIN_LED = 2;

// -------------------------------------------------------------- AJUSTE FINO
const int PWM_FREQ = 20000;   // 20 kHz: fora da faixa audivel, motor nao chia
const int PWM_RES  = 8;       // resolucao 8 bits -> 0 a 255

const int VEL_ATAQUE = 255;   // empurrando o oponente
const int VEL_BUSCA  = 165;   // girando a procura
const int VEL_RECUO  = 210;   // fugindo da borda
const int VEL_TESTE  = 140;   // teste de motor no MODO_TESTE

const int DIST_ALVO_CM  = 45; // acima disso, considera que nao ha oponente
const int CONFIRMA_ALVO = 2;  // leituras seguidas para aceitar a deteccao

const unsigned long T_PREPARO   = 5000;  // 5 s obrigatorios depois de ligar
const unsigned long T_RECUO     = 320;   // ms recuando ao ver a linha
const unsigned long T_GIRO_FUGA = 380;   // ms girando depois do recuo
const unsigned long T_VARRER    = 600;   // ms girando para cada lado na busca
const unsigned long T_PING      = 60;    // intervalo entre disparos do sonar

// ---------------------------------------------------------- SENSOR DE LINHA
// Leitura de 0 a 4095. Abaixo do limiar = BRANCO (borda).
// Com CALIBRAR_NO_INICIO = true o robo mede o preto durante os 5 s de espera
// e usa FRACAO_DO_PRETO dessa leitura como limiar de cada sensor.
const bool  CALIBRAR_NO_INICIO = true;
const int   LIMIAR_PADRAO      = 2000;   // usado sem calibracao ou se ela falhar
const float FRACAO_DO_PRETO    = 0.55;
const int   PRETO_MINIMO       = 1200;   // preto medido abaixo disso = algo errado

int limiarEsq = LIMIAR_PADRAO;
int limiarDir = LIMIAR_PADRAO;

// ------------------------------------------------------------ SONAR SEM TRAVA
// Le o HC-SR04 por interrupcao. O loop nunca fica parado esperando o eco,
// o que importa muito num sumo: 25 ms parado e o oponente ja te empurrou.
volatile unsigned long ecoInicio  = 0;
volatile unsigned long ecoDuracao = 0;
volatile bool          ecoPronto  = false;

void IRAM_ATTR trataEco() {
  if (digitalRead(PIN_ECHO)) {
    ecoInicio = micros();
  } else if (ecoInicio) {
    ecoDuracao = micros() - ecoInicio;
    ecoInicio  = 0;
    ecoPronto  = true;
  }
}

unsigned long ultimoPing = 0;
int distanciaCm = 999;
int alvoSeguido = 0;

void atualizaSonar() {
  unsigned long agora = millis();

  if (agora - ultimoPing >= T_PING) {
    ultimoPing = agora;
    ecoPronto  = false;
    digitalWrite(PIN_TRIG, LOW);
    delayMicroseconds(3);
    digitalWrite(PIN_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_TRIG, LOW);
  }

  if (ecoPronto) {
    ecoPronto = false;
    int d = ecoDuracao / 58;                 // us -> cm
    distanciaCm = (d < 2 || d > 400) ? 999 : d;
  } else if (agora - ultimoPing > T_PING * 3) {
    distanciaCm = 999;                       // sonar mudo: assume campo livre
  }

  // Exige leituras seguidas antes de confiar. Corta falso positivo de eco.
  if (distanciaCm <= DIST_ALVO_CM) {
    if (alvoSeguido < CONFIRMA_ALVO) alvoSeguido++;
  } else {
    alvoSeguido = 0;
  }
}

bool temAlvo() { return alvoSeguido >= CONFIRMA_ALVO; }

// ---------------------------------------------------------------- MOTORES
void acionaMotor(int in1, int in2, int pinPwm, int vel) {
  bool re = (vel < 0);
  int  v  = constrain(abs(vel), 0, 255);
  digitalWrite(in1, re ? LOW  : HIGH);
  digitalWrite(in2, re ? HIGH : LOW);
  ledcWrite(pinPwm, v);
}

void mover(int esq, int dir) {
  if (MODO_TESTE) return;                    // no teste, so gira quando voce manda
  acionaMotor(PIN_AIN1, PIN_AIN2, PIN_PWMA, esq);
  acionaMotor(PIN_BIN1, PIN_BIN2, PIN_PWMB, dir);
}

void parar() {
  acionaMotor(PIN_AIN1, PIN_AIN2, PIN_PWMA, 0);
  acionaMotor(PIN_BIN1, PIN_BIN2, PIN_PWMB, 0);
}

// ------------------------------------------------------------ SENSOR DE LINHA
int leLinha(int pino) {
  return (analogRead(pino) + analogRead(pino)) / 2;   // media de 2 corta ruido
}

bool linhaEsq() { return leLinha(PIN_LINHA_ESQ) < limiarEsq; }
bool linhaDir() { return leLinha(PIN_LINHA_DIR) < limiarDir; }

long somaEsq = 0, somaDir = 0;
int  amostras = 0;
unsigned long ultimaAmostra = 0;

void zeraCalibracao() { somaEsq = 0; somaDir = 0; amostras = 0; }

void guardaAmostra() {
  if (millis() - ultimaAmostra < 10 || amostras >= 250) return;
  ultimaAmostra = millis();
  somaEsq += leLinha(PIN_LINHA_ESQ);
  somaDir += leLinha(PIN_LINHA_DIR);
  amostras++;
}

int limiarDe(long soma, const char *lado) {
  int preto = amostras ? soma / amostras : 0;
  Serial.print(F("preto ")); Serial.print(lado); Serial.print(F(" = ")); Serial.print(preto);
  if (preto < PRETO_MINIMO) {
    Serial.println(F("  -> baixo demais, usando LIMIAR_PADRAO (confira fios e altura do sensor)"));
    return LIMIAR_PADRAO;
  }
  int limiar = constrain((int)(preto * FRACAO_DO_PRETO), 400, 3500);
  Serial.print(F("  -> limiar ")); Serial.println(limiar);
  return limiar;
}

void fechaCalibracao() {
  limiarEsq = limiarDe(somaEsq, "esq");
  limiarDir = limiarDe(somaDir, "dir");
}

// ---------------------------------------------------------- MAQUINA DE ESTADOS
enum Estado { PREPARANDO, BUSCANDO, ATACANDO, RECUANDO, GIRANDO_FUGA };

Estado        estado     = PREPARANDO;
unsigned long marcaTempo = 0;    // inicio do estado atual
int           ladoFuga   = 1;    // 1 = gira p/ direita, -1 = p/ esquerda
int           ladoBusca  = 1;

void trocaEstado(Estado novo) {
  estado     = novo;
  marcaTempo = millis();
}

// ------------------------------------------------------------- MODO TESTE
void testeMotor(bool esquerdo) {
  Serial.println(esquerdo ? F("motor ESQUERDO para frente") : F("motor DIREITO para frente"));
  if (esquerdo) acionaMotor(PIN_AIN1, PIN_AIN2, PIN_PWMA, VEL_TESTE);
  else          acionaMotor(PIN_BIN1, PIN_BIN2, PIN_PWMB, VEL_TESTE);
  delay(700);
  parar();
}

void rodaModoTeste() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'e') testeMotor(true);
    if (c == 'd') testeMotor(false);
    if (c == 'c') {                              // calibra agora, com o robo no preto
      zeraCalibracao();
      for (int i = 0; i < 60; i++) { ultimaAmostra = 0; guardaAmostra(); delay(5); }
      fechaCalibracao();
    }
  }

  static unsigned long ultimoLog = 0;
  if (millis() - ultimoLog < 300) return;
  ultimoLog = millis();
  int e = leLinha(PIN_LINHA_ESQ), d = leLinha(PIN_LINHA_DIR);
  Serial.print(F("dist ")); Serial.print(distanciaCm); Serial.print(F(" cm"));
  Serial.print(F(" | alvo ")); Serial.print(temAlvo() ? "SIM" : "nao");
  Serial.print(F(" | linha esq ")); Serial.print(e); Serial.print(e < limiarEsq ? " BRANCO" : " preto");
  Serial.print(F(" | dir "));       Serial.print(d); Serial.print(d < limiarDir ? " BRANCO" : " preto");
  Serial.print(F(" | limiar ")); Serial.print(limiarEsq); Serial.print('/'); Serial.println(limiarDir);
}

// --------------------------------------------------------------------- SETUP
void setup() {
  Serial.begin(115200);

  pinMode(PIN_AIN1, OUTPUT); pinMode(PIN_AIN2, OUTPUT);
  pinMode(PIN_BIN1, OUTPUT); pinMode(PIN_BIN2, OUTPUT);
  pinMode(PIN_STBY, OUTPUT); digitalWrite(PIN_STBY, HIGH);
  pinMode(PIN_LED,  OUTPUT);

  ledcAttach(PIN_PWMA, PWM_FREQ, PWM_RES);
  ledcAttach(PIN_PWMB, PWM_FREQ, PWM_RES);
  parar();

  pinMode(PIN_TRIG, OUTPUT); digitalWrite(PIN_TRIG, LOW);
  pinMode(PIN_ECHO, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_ECHO), trataEco, CHANGE);

  analogReadResolution(12);                  // 0 a 4095
  analogSetAttenuation(ADC_11db);            // faixa ate ~3,1 V

  Serial.println();
  Serial.println(F("Guerreiros de Realengo - pronto"));
  if (MODO_TESTE) {
    Serial.println(F("MODO TESTE: digite e / d para girar um motor, c para calibrar"));
  } else {
    Serial.println(F("MODO COMBATE: 5 s para comecar"));
  }
  zeraCalibracao();
  trocaEstado(PREPARANDO);                   // a contagem de 5 s comeca aqui
}

// ---------------------------------------------------------------------- LOOP
void loop() {
  atualizaSonar();

  // -------- modo teste: so relata --------
  if (MODO_TESTE) {
    rodaModoTeste();
    return;
  }

  unsigned long dt = millis() - marcaTempo;

  // -------- a borda tem prioridade sobre tudo --------
  // Sair do dohyo e derrota imediata. Nenhum ataque vale mais que isso.
  if (estado == BUSCANDO || estado == ATACANDO) {
    bool e = linhaEsq(), d = linhaDir();
    if (e || d) {
      ladoFuga = (e && !d) ? 1 : (d && !e) ? -1 : ladoFuga;
      trocaEstado(RECUANDO);
    }
  }

  switch (estado) {

    case PREPARANDO:
      parar();
      digitalWrite(PIN_LED, (millis() / 120) % 2);   // pisca rapido nos 5 s
      if (CALIBRAR_NO_INICIO && dt >= 1000 && dt < 4000) guardaAmostra();
      if (dt >= T_PREPARO) {
        if (CALIBRAR_NO_INICIO) fechaCalibracao();
        digitalWrite(PIN_LED, HIGH);
        trocaEstado(BUSCANDO);
      }
      break;

    case BUSCANDO:
      if (temAlvo()) {
        trocaEstado(ATACANDO);
        break;
      }
      // Gira no proprio eixo, alternando o lado, ate achar alguem.
      mover(VEL_BUSCA * ladoBusca, -VEL_BUSCA * ladoBusca);
      if (dt >= T_VARRER) {
        ladoBusca = -ladoBusca;
        marcaTempo = millis();
      }
      break;

    case ATACANDO:
      if (!temAlvo()) {
        ladoBusca = ladoFuga;          // continua procurando para onde ele foi
        trocaEstado(BUSCANDO);
        break;
      }
      mover(VEL_ATAQUE, VEL_ATAQUE);   // toca para cima, sem freio
      break;

    case RECUANDO:
      mover(-VEL_RECUO, -VEL_RECUO);
      if (dt >= T_RECUO) trocaEstado(GIRANDO_FUGA);
      break;

    case GIRANDO_FUGA:
      mover(VEL_RECUO * ladoFuga, -VEL_RECUO * ladoFuga);
      if (dt >= T_GIRO_FUGA) {
        ladoBusca = ladoFuga;
        trocaEstado(BUSCANDO);
      }
      break;
  }
}
