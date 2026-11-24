
`timescale 1ns/1ps

module tb_pwm_axi_lite;

    // ========================================================
    // CONFIGURACION
    // ========================================================

    localparam int CLK_HZ       = 100_000_000;
    localparam int CLK_NS       = 10;
    localparam int DEAD_CYCLES  = 20;  // 200 ns

    localparam logic [5:0] REG_CTRL = 6'h00;
    localparam logic [5:0] REG_REF  = 6'h04;
    localparam logic [5:0] REG_DEAD = 6'h08;
    localparam logic [5:0] REG_STAT = 6'h0C;

    localparam logic [1:0] OKAY   = 2'b00;
    localparam logic [1:0] SLVERR = 2'b10;

    // ========================================================
    // RELOJ Y RESET
    // ========================================================

    logic clk = 0;
    always #5 clk = ~clk;

    logic resetn = 0;

    // ========================================================
    // AXI4-LITE
    // ========================================================

    logic [5:0] awaddr = 0;
    logic [5:0] araddr = 0;

    logic awvalid = 0;
    logic awready;

    logic wvalid = 0;
    logic wready;

    logic [31:0] wdata = 0;
    logic [3:0]  wstrb = 0;

    logic bvalid;
    logic bready = 0;
    logic [1:0] bresp;

    logic arvalid = 0;
    logic arready;

    logic rvalid;
    logic rready = 0;

    logic [31:0] rdata;
    logic [1:0]  rresp;

    // ========================================================
    // SALIDAS PWM
    // ========================================================

    logic fault = 0;

    logic ah, al;
    logic bh, bl;
    logic trig;

    // ========================================================
    // DUT
    // ========================================================

    pwm_axi_lite dut (
        .aclk(clk),
        .aresetn(resetn),

        .s_axi_awaddr(awaddr),
        .s_axi_awprot(3'b000),
        .s_axi_awvalid(awvalid),
        .s_axi_awready(awready),

        .s_axi_wdata(wdata),
        .s_axi_wstrb(wstrb),
        .s_axi_wvalid(wvalid),
        .s_axi_wready(wready),

        .s_axi_bresp(bresp),
        .s_axi_bvalid(bvalid),
        .s_axi_bready(bready),

        .s_axi_araddr(araddr),
        .s_axi_arprot(3'b000),
        .s_axi_arvalid(arvalid),
        .s_axi_arready(arready),

        .s_axi_rdata(rdata),
        .s_axi_rresp(rresp),
        .s_axi_rvalid(rvalid),
        .s_axi_rready(rready),

        .fault_i(fault),

        .gate_ah_o(ah),
        .gate_al_o(al),
        .gate_bh_o(bh),
        .gate_bl_o(bl),

        .pwm_trigger_o(trig)
    );

    // ========================================================
    // ESTADISTICAS
    // ========================================================

    int checks = 0;
    int unsigned tick = 0;
    int unsigned trigger_count = 0;

    int edges_ah = 0;
    int edges_al = 0;
    int edges_bh = 0;
    int edges_bl = 0;

    int dead_checks_a = 0;
    int dead_checks_b = 0;

    bit monitor_deadtime = 0;

    bit prev_ah = 0;
    bit prev_al = 0;
    bit prev_bh = 0;
    bit prev_bl = 0;
    bit prev_trig = 0;

    int last_ah_fall = -1;
    int last_al_fall = -1;
    int last_bh_fall = -1;
    int last_bl_fall = -1;

    // ========================================================
    // MONITOR CONTINUO DE SEGURIDAD
    // Se muestrea en negedge para observar las salidas
    // despues de las actualizaciones del RTL.
    // ========================================================

    always @(negedge clk) begin

        if (!resetn) begin

            prev_ah = 0;
            prev_al = 0;
            prev_bh = 0;
            prev_bl = 0;
            prev_trig = 0;

        end else begin

            tick++;

            // Salidas desconocidas

            if ($isunknown({ah, al, bh, bl, trig}))
                $fatal(1,
                    "FAIL: salida PWM desconocida");

            // Shoot-through

            if (ah && al)
                $fatal(1,
                    "FAIL: shoot-through rama A");

            if (bh && bl)
                $fatal(1,
                    "FAIL: shoot-through rama B");

            // Trigger de un solo reloj

            if (trig && prev_trig)
                $fatal(1,
                    "FAIL: trigger dura mas de un ciclo");

            if (trig) begin

                trigger_count++;

                // Comprobacion interna de fase.
                // Especifica para este RTL.

                if (dut.running &&
                    dut.phase !=
                    (dut.period_active / 2) + 1)
                begin
                    $fatal(1,
                        "FAIL: trigger fuera del centro");
                end
            end

            // ================================================
            // VERIFICACION DE TIEMPO MUERTO
            // ================================================

            if (monitor_deadtime) begin

                // Registrar apagado de cada transistor

                if (prev_ah && !ah)
                    last_ah_fall = tick;

                if (prev_al && !al)
                    last_al_fall = tick;

                if (prev_bh && !bh)
                    last_bh_fall = tick;

                if (prev_bl && !bl)
                    last_bl_fall = tick;

                // Encendido AH despues de apagar AL

                if (!prev_ah && ah) begin

                    edges_ah++;

                    if (last_al_fall >= 0) begin

                        if ((tick - last_al_fall) <
                            DEAD_CYCLES)
                        begin
                            $fatal(1,
                                "FAIL: deadtime AL->AH");
                        end

                        dead_checks_a++;
                    end
                end

                // Encendido AL despues de apagar AH

                if (!prev_al && al) begin

                    edges_al++;

                    if (last_ah_fall >= 0) begin

                        if ((tick - last_ah_fall) <
                            DEAD_CYCLES)
                        begin
                            $fatal(1,
                                "FAIL: deadtime AH->AL");
                        end

                        dead_checks_a++;
                    end
                end

                // Encendido BH despues de apagar BL

                if (!prev_bh && bh) begin

                    edges_bh++;

                    if (last_bl_fall >= 0) begin

                        if ((tick - last_bl_fall) <
                            DEAD_CYCLES)
                        begin
                            $fatal(1,
                                "FAIL: deadtime BL->BH");
                        end

                        dead_checks_b++;
                    end
                end

                // Encendido BL despues de apagar BH

                if (!prev_bl && bl) begin

                    edges_bl++;

                    if (last_bh_fall >= 0) begin

                        if ((tick - last_bh_fall) <
                            DEAD_CYCLES)
                        begin
                            $fatal(1,
                                "FAIL: deadtime BH->BL");
                        end

                        dead_checks_b++;
                    end
                end

            end else begin

                last_ah_fall = -1;
                last_al_fall = -1;
                last_bh_fall = -1;
                last_bl_fall = -1;

            end

            prev_ah = ah;
            prev_al = al;
            prev_bh = bh;
            prev_bl = bl;
            prev_trig = trig;

        end
    end

    // ========================================================
    // TAREA AXI WRITE
    //
    // Permite AW y W en ciclos distintos.
    // Comprueba tambien que BVALID se mantenga
    // mientras BREADY = 0.
    // ========================================================

    task automatic axi_write(
        input logic [5:0] addr,
        input logic [31:0] value,
        input logic [3:0] strobe = 4'hF,
        input logic [1:0] expected = OKAY,
        input int aw_delay = 0,
        input int w_delay = 0
    );

        bready = 0;

        fork

            begin
                repeat (aw_delay + 1)
                    @(negedge clk);

                awaddr = addr;
                awvalid = 1;

                do @(posedge clk);
                while (awready !== 1'b1);

                @(negedge clk);
                awvalid = 0;
            end

            begin
                repeat (w_delay + 1)
                    @(negedge clk);

                wdata = value;
                wstrb = strobe;
                wvalid = 1;

                do @(posedge clk);
                while (wready !== 1'b1);

                @(negedge clk);
                wvalid = 0;
            end

        join

        wait (bvalid === 1'b1);

        // Probar backpressure

        repeat (2) begin

            @(negedge clk);

            if (bvalid !== 1'b1)
                $fatal(1,
                    "FAIL: BVALID no se mantiene");
        end

        if (bresp !== expected)
            $fatal(1,
                "FAIL: BRESP addr=%h esperado=%b real=%b",
                addr, expected, bresp);

        bready = 1;

        @(posedge clk);
        @(negedge clk);

        bready = 0;

        checks++;

    endtask

    // ========================================================
    // TAREA AXI READ
    //
    // Comprueba RVALID, RRESP y estabilidad
    // del dato durante backpressure.
    // ========================================================

    task automatic axi_read(
        input logic [5:0] addr,
        output logic [31:0] value,
        input logic [1:0] expected = OKAY
    );

        logic [31:0] held_data;

        @(negedge clk);

        araddr = addr;
        arvalid = 1;
        rready = 0;

        do @(posedge clk);
        while (arready !== 1'b1);

        @(negedge clk);
        arvalid = 0;

        wait (rvalid === 1'b1);

        held_data = rdata;

        repeat (2) begin

            @(negedge clk);

            if (rvalid !== 1'b1)
                $fatal(1,
                    "FAIL: RVALID no se mantiene");

            if (rdata !== held_data)
                $fatal(1,
                    "FAIL: RDATA inestable");
        end

        if (rresp !== expected)
            $fatal(1,
                "FAIL: RRESP addr=%h esperado=%b real=%b",
                addr, expected, rresp);

        value = rdata;

        rready = 1;

        @(posedge clk);
        @(negedge clk);

        rready = 0;

        checks++;

    endtask

    // ========================================================
    // COMPROBAR FRECUENCIA Y TRIGGER
    //
    // Descarta pulsos iniciales durante cambios
    // de frecuencia y mide el periodo estable.
    // ========================================================

    task automatic check_period(
        input int expected_cycles,
        input string label_name
    );

        time t0, t1, t2;

        repeat (3) @(posedge trig);

        @(posedge trig);
        t0 = $time;

        @(posedge trig);
        t1 = $time;

        @(posedge trig);
        t2 = $time;

        if ((t1 - t0) !=
            (expected_cycles * CLK_NS))
        begin
            $fatal(1,
                "FAIL %s: periodo 1 esperado=%0d ns real=%0t ns",
                label_name,
                expected_cycles * CLK_NS,
                t1 - t0);
        end

        if ((t2 - t1) !=
            (expected_cycles * CLK_NS))
        begin
            $fatal(1,
                "FAIL %s: periodo 2 incorrecto",
                label_name);
        end

        $display(
            "PASS frecuencia %s: %0d ciclos",
            label_name,
            expected_cycles
        );

        checks++;

    endtask

    // ========================================================
    // COMPROBAR MODULACION UNIPOLAR
    //
    // Se comparan los tiempos de conduccion
    // de los transistores superiores A y B.
    //
    // +ref: A conduce mas que B
    // -ref: B conduce mas que A
    // 0ref: conducciones aproximadamente iguales
    // ========================================================

    task automatic check_modulation(
        input logic [15:0] ref_value,
        input integer expected_sign
    );

        int count_a;
        int count_b;
        int difference;

        axi_write(REG_REF, {16'd0, ref_value});

        // Esperar actualizacion de referencia

        repeat (3) @(posedge trig);

        count_a = 0;
        count_b = 0;

        repeat (5000) begin

            @(negedge clk);

            if (ah) count_a++;
            if (bh) count_b++;

        end

        difference = count_a - count_b;

        if (expected_sign > 0) begin

            if (difference <= 500)
                $fatal(1,
                    "FAIL: modulacion positiva");

        end else if (expected_sign < 0) begin

            if (difference >= -500)
                $fatal(1,
                    "FAIL: modulacion negativa");

        end else begin

            if (difference > 200 ||
                difference < -200)
            begin
                $fatal(1,
                    "FAIL: modulacion cero");
            end
        end

        $display(
            "PASS modulacion ref=0x%h A=%0d B=%0d",
            ref_value,
            count_a,
            count_b
        );

        checks++;

    endtask

    // ========================================================
    // PRUEBA PRINCIPAL
    // ========================================================

    initial begin : main_test

        logic [31:0] v;

        $display("");
        $display("======================================");
        $display(" VALIDACION PWM AXI4-LITE");
        $display(" CLK = 100 MHz");
        $display(" DEADTIME = 200 ns");
        $display("======================================");

        // ----------------------------------------------------
        // TEST 1: RESET
        // ----------------------------------------------------

        repeat (8) @(negedge clk);
        resetn = 1;

        axi_read(REG_CTRL, v);

        if (v[0] !== 0 || v[3] !== 0)
            $fatal(1,
                "FAIL: PWM activo tras reset");

        if ({ah, al, bh, bl} !== 4'b0000)
            $fatal(1,
                "FAIL: gates activos en reset");

        axi_read(REG_DEAD, v);

        if (v[7:0] !== 20)
            $fatal(1,
                "FAIL: deadtime por defecto");

        $display("PASS reset");

        // ----------------------------------------------------
        // TEST 2: REGISTRO DEADTIME
        // ----------------------------------------------------

        axi_write(REG_DEAD, 32'd0);
        axi_read(REG_DEAD, v);

        if (v[7:0] !== 1)
            $fatal(1,
                "FAIL: saturacion deadtime minimo");

        axi_write(REG_DEAD, 32'd150);
        axi_read(REG_DEAD, v);

        if (v[7:0] !== 100)
            $fatal(1,
                "FAIL: saturacion deadtime maximo");

        axi_write(REG_DEAD, DEAD_CYCLES);
        axi_read(REG_DEAD, v);

        if (v[7:0] !== DEAD_CYCLES)
            $fatal(1,
                "FAIL: deadtime 200 ns");

        $display("PASS deadtime configuracion");

        // ----------------------------------------------------
        // TEST 3: SATURACION Q1.15
        // ----------------------------------------------------

        axi_write(REG_REF, 32'h00008000);
        axi_read(REG_REF, v);

        if (v[15:0] !== 16'h8001)
            $fatal(1,
                "FAIL: saturacion Q1.15");

        axi_write(REG_REF, 32'h00007FFF);
        axi_read(REG_REF, v);

        if (v[15:0] !== 16'h7FFF)
            $fatal(1,
                "FAIL: referencia maxima");

        $display("PASS saturacion Q1.15");

        // ----------------------------------------------------
        // TEST 4: WSTRB Y CANALES AXI SEPARADOS
        // ----------------------------------------------------

        // Direccion antes que datos

        axi_write(
            REG_REF,
            32'h00001234,
            4'hF,
            OKAY,
            0,
            4
        );

        // Datos antes que direccion

        axi_write(
            REG_REF,
            32'h000000AA,
            4'b0001,
            OKAY,
            4,
            0
        );

        axi_read(REG_REF, v);

        if (v[15:0] !== 16'h12AA)
            $fatal(1,
                "FAIL: WSTRB parcial");

        // Ningun byte habilitado

        axi_write(
            REG_REF,
            32'h0000FFFF,
            4'b0000
        );

        axi_read(REG_REF, v);

        if (v[15:0] !== 16'h12AA)
            $fatal(1,
                "FAIL: WSTRB=0 modifico registro");

        $display("PASS AXI separado y WSTRB");

        // ----------------------------------------------------
        // TEST 5: DIRECCIONES INVALIDAS
        // ----------------------------------------------------

        axi_write(
            6'h10,
            32'h12345678,
            4'hF,
            SLVERR
        );

        axi_read(6'h10, v, SLVERR);

        $display("PASS accesos invalidos");

        // ----------------------------------------------------
        // TEST 6: ENABLE Y FRECUENCIAS
        // ----------------------------------------------------

        axi_write(REG_REF, 32'd0);

        // 10 kHz: control = 001

        axi_write(REG_CTRL, 32'h1);
        axi_read(REG_CTRL, v);

        if (v[3] !== 1)
            $fatal(1,
                "FAIL: running no se activa");

        monitor_deadtime = 1;

        check_period(10000, "10 kHz");

        // 12.5 kHz: control = 011

        axi_write(REG_CTRL, 32'h3);
        check_period(8000, "12.5 kHz");

        // 20 kHz: control = 101

        axi_write(REG_CTRL, 32'h5);
        check_period(5000, "20 kHz");

        // ----------------------------------------------------
        // TEST 7: SELECTOR RESERVADO
        // ----------------------------------------------------

        axi_write(
            REG_CTRL,
            32'h7,
            4'hF,
            SLVERR
        );

        axi_read(REG_CTRL, v);

        if (v[2:1] !== 2'b10)
            $fatal(1,
                "FAIL: selector reservado altero frecuencia");

        if (v[3] !== 1)
            $fatal(1,
                "FAIL: selector reservado apago PWM");

        $display("PASS selector reservado");

        // ----------------------------------------------------
        // TEST 8: MODULACION
        // ----------------------------------------------------

        check_modulation(16'h4000,  1); // +0.5
        check_modulation(16'hC000, -1); // -0.5
        check_modulation(16'h0000,  0); //  0.0

        // ----------------------------------------------------
        // TEST 9: CONMUTACION REAL
        // ----------------------------------------------------

        if (edges_ah == 0 ||
            edges_al == 0 ||
            edges_bh == 0 ||
            edges_bl == 0)
        begin
            $fatal(1,
                "FAIL: uno o mas gates nunca conmutaron");
        end

        if (dead_checks_a < 2 ||
            dead_checks_b < 2)
        begin
            $fatal(1,
                "FAIL: cobertura insuficiente deadtime");
        end

        $display(
            "PASS gates: AH=%0d AL=%0d BH=%0d BL=%0d",
            edges_ah,
            edges_al,
            edges_bh,
            edges_bl
        );

        $display(
            "PASS deadtime: rama A=%0d rama B=%0d",
            dead_checks_a,
            dead_checks_b
        );

        // ----------------------------------------------------
        // TEST 10: FALLA ASINCRONA
        // ----------------------------------------------------

        // Esperar que haya una salida encendida

        do @(negedge clk);
        while (!(ah || al || bh || bl));

        monitor_deadtime = 0;

        fault = 1;

        #1;

        if ({ah, al, bh, bl} !== 4'b0000)
            $fatal(1,
                "FAIL: gates no se apagan ante fault");

        repeat (3) @(negedge clk);

        axi_read(REG_STAT, v);

        if (v[0] !== 1)
            $fatal(1,
                "FAIL: fault no enclavado");

        axi_read(REG_CTRL, v);

        if (v[3] !== 0)
            $fatal(1,
                "FAIL: running sigue activo en fault");

        $display("PASS falla y enclavamiento");

        // ----------------------------------------------------
        // TEST 11: NO REARRANQUE AUTOMATICO
        // ----------------------------------------------------

        fault = 0;

        repeat (5) @(negedge clk);

        axi_read(REG_CTRL, v);

        if (v[3] !== 0)
            $fatal(1,
                "FAIL: PWM reinicia sin limpiar falla");

        if ({ah, al, bh, bl} !== 4'b0000)
            $fatal(1,
                "FAIL: gates activos con falla enclavada");

        $display("PASS no rearranque automatico");

        // ----------------------------------------------------
        // TEST 12: LIMPIEZA DE FALLA
        // ----------------------------------------------------

        // Deshabilitar conservando freq_sel=2

        axi_write(REG_CTRL, 32'h4);

        axi_read(REG_STAT, v);

        if (v[0] !== 0)
            $fatal(1,
                "FAIL: falla no se limpia");

        axi_read(REG_CTRL, v);

        if (v[3] !== 0)
            $fatal(1,
                "FAIL: running no se deshabilita");

        // Trigger debe quedar apagado

        repeat (5) @(negedge clk);

        if (trig !== 0)
            $fatal(1,
                "FAIL: trigger activo con PWM apagado");

        $display("PASS limpieza de falla");

        // ----------------------------------------------------
        // TEST 13: REHABILITACION
        // ----------------------------------------------------

        axi_write(REG_CTRL, 32'h5);

        axi_read(REG_CTRL, v);

        if (v[3] !== 1)
            $fatal(1,
                "FAIL: PWM no rehabilita");

        check_period(5000, "20 kHz rehabilitado");

        axi_write(REG_CTRL, 32'h4);

        repeat (5) @(negedge clk);

        if ({ah, al, bh, bl} !== 4'b0000)
            $fatal(1,
                "FAIL: gates activos tras disable");

        $display("PASS rehabilitacion y parada");

        // ----------------------------------------------------
        // RESUMEN
        // ----------------------------------------------------

        $display("");
        $display("======================================");
        $display(" VALIDACION PWM COMPLETADA");
        $display("======================================");

        $display(
            "Operaciones y verificaciones: %0d",
            checks
        );

        $display(
            "Pulsos trigger detectados: %0d",
            trigger_count
        );

        $display(
            "Conmutaciones AH/AL: %0d/%0d",
            edges_ah,
            edges_al
        );

        $display(
            "Conmutaciones BH/BL: %0d/%0d",
            edges_bh,
            edges_bl
        );

        $display("Resultado: PASS");
        $display("======================================");

        $finish;

    end

    // ========================================================
    // TIMEOUT GLOBAL
    // ========================================================

    initial begin

        #20_000_000;

        $fatal(1,
            "TIMEOUT: simulacion no finalizo");

    end

endmodule
