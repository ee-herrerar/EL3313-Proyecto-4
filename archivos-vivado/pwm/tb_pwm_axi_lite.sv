`timescale 1ns/1ps
module tb_pwm_axi_lite;
    logic clk = 0;
    always #5 clk = ~clk; 
    logic resetn = 0;
    logic [5:0] awaddr = 0, araddr = 0;
    logic awvalid = 0, awready, wvalid = 0, wready, bvalid, bready = 0;
    logic [31:0] wdata = 0, rdata;
    logic [3:0] wstrb = 0;
    logic [1:0] bresp, rresp;
    logic arvalid = 0, arready, rvalid, rready = 0;
    logic fault = 0, ah,al,bh,bl,trig;
    int unsigned cycle_no = 0;
    int unsigned prev_a_off = 0, prev_b_off = 0;
    bit seen_a_off = 0, seen_b_off = 0;
    bit prev_ah = 0, prev_al = 0, prev_bh = 0, prev_bl = 0;
    int checks = 0;

    pwm_axi_lite dut (
        .aclk(clk), .aresetn(resetn),
        .s_axi_awaddr(awaddr), .s_axi_awprot(3'b000), .s_axi_awvalid(awvalid), .s_axi_awready(awready),
        .s_axi_wdata(wdata), .s_axi_wstrb(wstrb), .s_axi_wvalid(wvalid),
        .s_axi_wready(wready), .s_axi_bresp(bresp), .s_axi_bvalid(bvalid), .s_axi_bready(bready),
        .s_axi_araddr(araddr), .s_axi_arprot(3'b000), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
        .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid), .s_axi_rready(rready),
        .fault_i(fault), .gate_ah_o(ah), .gate_al_o(al), .gate_bh_o(bh), .gate_bl_o(bl),
        .pwm_trigger_o(trig)
    );

    always @(negedge clk) if (resetn) begin
        cycle_no++;
        if (ah && al) $fatal(1,"Shoot-through rama A");
        if (bh && bl) $fatal(1,"Shoot-through rama B");
        if ((prev_ah || prev_al) && !ah && !al) begin
            seen_a_off = 1;
            prev_a_off = cycle_no;
        end
        if ((prev_bh || prev_bl) && !bh && !bl) begin
            seen_b_off = 1;
            prev_b_off = cycle_no;
        end
        if (seen_a_off && (ah || al)) begin
            if ((cycle_no - prev_a_off) < 5)
                $fatal(1,"Tiempo muerto insuficiente rama A");
            seen_a_off = 0;
        end
        if (seen_b_off && (bh || bl)) begin
            if ((cycle_no - prev_b_off) < 5)
                $fatal(1,"Tiempo muerto insuficiente rama B");
            seen_b_off = 0;
        end
        prev_ah = ah; prev_al = al; prev_bh = bh; prev_bl = bl;
    end

    task automatic axi_write(input logic[5:0] addr, input logic[31:0] value,
                              input logic[3:0] strobe=4'hf, input logic[1:0] expected=2'b00);
        @(negedge clk);
        awaddr=addr; awvalid=1; wdata=value; wstrb=strobe; wvalid=1; bready=1;
        do @(posedge clk); while (!awready || !wready);
        @(negedge clk); awvalid=0; wvalid=0;
        do @(posedge clk); while (!bvalid);
        if (bresp!==expected) $fatal(1,"BRESP addr=%h expected=%b actual=%b", addr,expected,bresp);
        @(negedge clk); bready=0;
        checks++;
    endtask

    task automatic axi_read(input logic[5:0] addr, output logic [31:0] value);
        @(negedge clk);
        araddr=addr; arvalid=1; rready=1;
        do @(posedge clk); while (!arready);
        @(negedge clk); arvalid=0;
        do @(posedge clk); while (!rvalid);
        if (rresp !== 2'b00) $fatal(1,"RRESP no OKAY: addr=%h",addr);
        value = rdata;
        @(negedge clk); rready=0;
        checks++;
    endtask

    task automatic check_period(input int expected);
        int t0,t1,t2;
        @(posedge trig); t0=cycle_no;
        @(posedge trig); t1=cycle_no;
        @(posedge trig); t2=cycle_no;
        if ((t2-t1)!=expected) $fatal(1,"Periodo esperado %0d, medido %0d",expected,t2-t1);
        $display("PASS frecuencia: periodo=%0d ciclos (%0f Hz)",expected,100000000.0/expected);
        checks++;
    endtask

    initial begin : main_test
        logic [31:0] v;
        repeat(8) @(negedge clk);
        resetn=1;
        axi_read(6'h00,v);
        if (v[0]!==0 || v[3]!==0) $fatal(1,"PWM debía estar apagado en reset");
        axi_write(6'h08,32'd5);
        axi_write(6'h04,32'h00008000); // -32768 se satura a -32767
        axi_read(6'h04,v);
        if (v[15:0]!==16'h8001) $fatal(1,"Falla saturación Q1.15");
        axi_write(6'h04,32'h00000000);
        axi_write(6'h00,32'h00000001);
        axi_read(6'h00,v);
        if (v[3]!==1) $fatal(1,"running no se activa");
        check_period(10000);
        axi_write(6'h00,32'h00000003);
        check_period(8000);
        axi_write(6'h00,32'h00000005);
        check_period(5000);
    
        axi_write(6'h00,32'h00000007,4'hf,2'b10);
        axi_read(6'h00,v);
        if (v[2:1]!==2'b10) $fatal(1,"Selector reservado cambió el estado");
        @(negedge clk); fault=1;
        #1;
        if (ah || al || bh || bl) $fatal(1,"Los gates no se apagan inmediatamente ante fault");
        repeat (3) @(negedge clk);
        axi_read(6'h0C,v);
        if (v[0]!==1) $fatal(1,"Falla no queda enclavada");
        fault=0;
        axi_write(6'h00,32'h00000004); 
        axi_read(6'h00,v);
        if (v[3]!==0) $fatal(1,"running no baja");
        axi_read(6'h0C,v);
        if (v[0]!==0) $fatal(1,"Falla no se reinicia tras deshabilitar");
        $display("PASS: %0d operaciones/checks, sin shoot-through",checks);
        $finish;
    end
    initial begin #10_000_000; $fatal(1,"TIMEOUT testbench"); end
endmodule
