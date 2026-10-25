# EL3313-Proyecto 4: Reloj Digital Embebido con MicroBlaze V Sistema de Alarma y Cron´ometro


## Fases y Módulos de la Etapa Digital
### RISC-V Microblaze 
La arquitectura planteada sigue la siguiente lógica
```mermaid
flowchart TD
    A["Etapa de potencia"] --> B["Sensores de tensión y corriente"]
    B --> C["ADC / adquisición"]
    C --> D["Registros AXI de medición"]
    D --> E["MicroBlaze V\nControlador PR en C"]
    E --> F["Registros AXI de referencia"]
    F --> G["PWM en SystemVerilog"]
    G --> H["Salidas de control hacia drivers"]
    H --> A
```

```mermaid
flowchart TD
    PC["Computadora<br/>Terminal serial / Vitis"]
    UART["AXI UART Lite<br/>115200 baudios"]
    CPU["MicroBlaze V<br/>100 MHz"]
    TIMER["AXI Timer<br/>Interrupción periódica"]
    INTC["AXI Interrupt Controller"]
    PWM["Futuro PWM<br/>SystemVerilog"]
    BRAM["BRAM local<br/>32 KB"]

    PC <-->|USB-UART| UART
    UART <-->|AXI4-Lite| CPU
    CPU <-->|ILMB / DLMB| BRAM
    CPU <-->|AXI4-Lite| TIMER
    TIMER -->|Interrupt| INTC
    INTC -->|Interrupt| CPU
    CPU -.->|Registro AXI futuro| PWM
```

#### AXI Timer
```mermaid
flowchart TD
    MB["MicroBlaze V"]
    AXI["AXI SmartConnect"]
    TIMER["AXI Timer"]
    UART["AXI UART Lite"]
    PC["PC / Terminal serial"]
    INT["AXI Interrupt Controller"]

    MB -->|M_AXI_DP| AXI
    AXI --> TIMER
    AXI --> UART
    UART <-->|USB-UART| PC
    TIMER -.->|Pendiente de conectar| INT
    INT -->|Interrupt| MB
```