module pwm_axi_lite #(
    parameter int unsigned CLK_HZ = 100_000_000
) (
    input  logic        aclk,
    input  logic        aresetn,
    input  logic [5:0]  s_axi_awaddr,
    input  logic [2:0]  s_axi_awprot,  
    input  logic        s_axi_awvalid,
    output logic        s_axi_awready,
    input  logic [31:0] s_axi_wdata,
    input  logic [3:0]  s_axi_wstrb,
    input  logic        s_axi_wvalid,
    output logic        s_axi_wready,
    output logic [1:0]  s_axi_bresp,
    output logic        s_axi_bvalid,
    input  logic        s_axi_bready,
    input  logic [5:0]  s_axi_araddr,
    input  logic [2:0]  s_axi_arprot, 
    input  logic        s_axi_arvalid,
    output logic        s_axi_arready,
    output logic [31:0] s_axi_rdata,
    output logic [1:0]  s_axi_rresp,
    output logic        s_axi_rvalid,
    input  logic        s_axi_rready,
    input  logic        fault_i,
    output logic        gate_ah_o, gate_al_o,
    output logic        gate_bh_o, gate_bl_o,
    output logic        pwm_trigger_o
);

    localparam logic [1:0] RESP_OKAY = 2'b00;
    localparam logic [1:0] RESP_SLVERR = 2'b10;

    logic [5:0]  awaddr_q;
    logic [31:0] wdata_q;
    logic [3:0]  wstrb_q;
    logic        aw_pending, w_pending;
    logic        enable_q;
    logic [1:0]  freq_sel_q;
    logic signed [15:0] ref_q;
    logic [7:0]  deadtime_q;
    logic        fault_latched;

    logic [15:0] period_active, period_requested;
    logic [15:0] phase;
    logic signed [15:0] ref_active;
    logic [7:0]  deadtime_active;
    logic [15:0] cmp_a, cmp_b;
    logic [15:0] carrier;
    logic target_a, target_b;
    logic raw_ah, raw_al, raw_bh, raw_bl;
    logic running;

    function automatic [15:0] period_for(input logic [1:0] sel);
        case (sel)
            2'b00: period_for = CLK_HZ / 10000;
            2'b01: period_for = CLK_HZ / 12500;
            2'b10: period_for = CLK_HZ / 20000;
            default: period_for = CLK_HZ / 10000;
        endcase
    endfunction

    function automatic [31:0] apply_wstrb(
        input logic [31:0] prior,
        input logic [31:0] value,
        input logic [3:0]  strobe
    );
        logic [31:0] v;
        v = prior;
        for (int i=0; i<4; i++)
            if (strobe[i]) v[i*8 +: 8] = value[i*8 +: 8];
        return v;
    endfunction

    function automatic logic signed [15:0] clamp_ref(input logic signed [15:0] q);
        return (q == -16'sd32768) ? -16'sd32767 : q;
    endfunction

    assign s_axi_awready = !aw_pending && !s_axi_bvalid;
    assign s_axi_wready  = !w_pending  && !s_axi_bvalid;
    assign s_axi_arready = !s_axi_rvalid;
    assign running       = enable_q && !fault_latched && !fault_i;
    assign period_requested = period_for(freq_sel_q);

    always_ff @(posedge aclk) begin : axi_regs
        logic [31:0] merged;
        if (!aresetn) begin
            aw_pending <= 1'b0;
            w_pending <= 1'b0;
            awaddr_q <= '0;
            wdata_q <= '0;
            wstrb_q <= '0;
            s_axi_bresp <= RESP_OKAY;
            s_axi_bvalid <= 1'b0;
            s_axi_rdata <= '0;
            s_axi_rresp <= RESP_OKAY;
            s_axi_rvalid <= 1'b0;
            enable_q <= 1'b0;
            freq_sel_q <= 2'b00;
            ref_q <= '0;
            deadtime_q <= 8'd20;
            fault_latched <= 1'b0;
        end else begin
            if (fault_i) fault_latched <= 1'b1;
            if (s_axi_awvalid && s_axi_awready) begin
                awaddr_q <= s_axi_awaddr;
                aw_pending <= 1'b1;
            end
            if (s_axi_wvalid && s_axi_wready) begin
                wdata_q <= s_axi_wdata;
                wstrb_q <= s_axi_wstrb;
                w_pending <= 1'b1;
            end
            if (aw_pending && w_pending && !s_axi_bvalid) begin
                s_axi_bvalid <= 1'b1;
                s_axi_bresp <= RESP_OKAY;
                aw_pending <= 1'b0;
                w_pending <= 1'b0;
                case (awaddr_q)
                    6'h00: begin
                        merged = apply_wstrb({29'd0,freq_sel_q,enable_q}, wdata_q, wstrb_q);
                        if (merged[2:1] == 2'b11) begin
                            s_axi_bresp <= RESP_SLVERR;
                        end else begin
                            enable_q <= merged[0];
                            freq_sel_q <= merged[2:1];
                            if (!merged[0] && !fault_i)
                                fault_latched <= 1'b0; // reset explícito de falla
                        end
                    end
                    6'h04: begin
                        merged = apply_wstrb({16'd0,ref_q}, wdata_q, wstrb_q);
                        ref_q <= clamp_ref(merged[15:0]);
                    end
                    6'h08: begin
                        merged = apply_wstrb({24'd0,deadtime_q}, wdata_q, wstrb_q);
                        if (merged[7:0] == 8'd0) deadtime_q <= 8'd1;
                        else if (merged[7:0] > 8'd100) deadtime_q <= 8'd100;
                        else deadtime_q <= merged[7:0];
                    end
                    default: s_axi_bresp <= RESP_SLVERR;
                endcase
            end
            if (s_axi_bvalid && s_axi_bready) s_axi_bvalid <= 1'b0;

            if (s_axi_arvalid && s_axi_arready) begin
                s_axi_rvalid <= 1'b1;
                s_axi_rresp <= RESP_OKAY;
                case (s_axi_araddr)
                    6'h00: s_axi_rdata <= {28'd0,running,freq_sel_q,enable_q};
                    6'h04: s_axi_rdata <= {16'd0,ref_q};
                    6'h08: s_axi_rdata <= {24'd0,deadtime_q};
                    6'h0C: s_axi_rdata <= {30'd0,fault_i,fault_latched};
                    default: begin
                        s_axi_rdata <= '0;
                        s_axi_rresp <= RESP_SLVERR;
                    end
                endcase
            end
            if (s_axi_rvalid && s_axi_rready) s_axi_rvalid <= 1'b0;
        end
    end

    always_comb begin : mod_calc
        logic signed [16:0] biased;
        logic [32:0] product;
        biased = $signed({ref_active[15],ref_active}) + 17'sd32768;
        product = period_active * $unsigned(biased);
        cmp_a = product >> 16;
        cmp_b = period_active - cmp_a;
        if (phase < period_active/2) carrier = phase << 1;
        else carrier = (period_active - phase) << 1;
        target_a = (carrier < cmp_a);
        target_b = (carrier < cmp_b);
    end

    always_ff @(posedge aclk) begin
        if (!aresetn || !running) begin
            phase <= '0;
            period_active <= CLK_HZ/10000;
            ref_active <= '0;
            deadtime_active <= 8'd20;
            pwm_trigger_o <= 1'b0;
        end else begin
            pwm_trigger_o <= (phase == (period_active/2));
            if (phase == period_active-1) begin
                phase <= '0;
                period_active <= period_requested;
                ref_active <= ref_q;
                deadtime_active <= deadtime_q;
            end else phase <= phase + 1'b1;
        end
    end


    deadtime_leg u_leg_a (
        .clk(aclk), .resetn(aresetn), .enable(running),
        .target_high(target_a), .dead_clocks(deadtime_active),
        .high_o(raw_ah), .low_o(raw_al)
    );
    deadtime_leg u_leg_b (
        .clk(aclk), .resetn(aresetn), .enable(running),
        .target_high(target_b), .dead_clocks(deadtime_active),
        .high_o(raw_bh), .low_o(raw_bl)
    );


    assign gate_ah_o = running && raw_ah;
    assign gate_al_o = running && raw_al;
    assign gate_bh_o = running && raw_bh;
    assign gate_bl_o = running && raw_bl;
endmodule

module deadtime_leg (
    input  logic clk, resetn, enable, target_high,
    input  logic [7:0] dead_clocks,
    output logic high_o, low_o
);
    logic [7:0] wait_q;

    always_ff @(posedge clk) begin
        if (!resetn || !enable) begin
            high_o <= 1'b0;
            low_o <= 1'b0;
            wait_q <= 8'd0;
        end else if (high_o && !target_high) begin
            high_o <= 1'b0;
            low_o <= 1'b0;
            wait_q <= dead_clocks;
        end else if (low_o && target_high) begin
            high_o <= 1'b0;
            low_o <= 1'b0;
            wait_q <= dead_clocks;
        end else if (!high_o && !low_o) begin
            if (wait_q != 0) wait_q <= wait_q - 1'b1;
            else begin
                high_o <= target_high;
                low_o <= !target_high;
            end
        end
    end
endmodule
