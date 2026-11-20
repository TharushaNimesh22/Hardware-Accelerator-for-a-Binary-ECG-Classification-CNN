module soc #(
    parameter CLKS_PER_BIT = 434
)(
    input         clk,
    input         rst,
    output        uart_tx,
    input         uart_rx,
    output reg [17:0] ledr,
    output reg [8:0]  ledg
);
    //  CPU
    wire        mem_valid, mem_instr;
    reg         mem_ready;
    wire [31:0] mem_addr, mem_wdata;
    wire [3:0]  mem_wstrb;
    reg  [31:0] mem_rdata;

    picorv32 #(
        .ENABLE_COUNTERS(1), .ENABLE_MUL(0), .ENABLE_FAST_MUL(1), .ENABLE_DIV(1), .BARREL_SHIFTER(1),
        .PROGADDR_RESET(32'h0000_0000), .STACKADDR(32'h0001_0000)
    ) cpu (
        .clk(clk), .resetn(~rst), .trap(),
        .mem_valid(mem_valid), .mem_instr(mem_instr), .mem_ready(mem_ready),
        .mem_addr(mem_addr), .mem_wdata(mem_wdata), .mem_wstrb(mem_wstrb), .mem_rdata(mem_rdata),
        .mem_la_read(), .mem_la_write(), .mem_la_addr(), .mem_la_wdata(), .mem_la_wstrb(),
        .pcpi_valid(), .pcpi_insn(), .pcpi_rs1(), .pcpi_rs2(),
        .pcpi_wr(1'b0), .pcpi_rd(32'd0), .pcpi_wait(1'b0), .pcpi_ready(1'b0),
        .irq(32'd0), .eoi(), .trace_valid(), .trace_data()
    );

    //  address decode
    wire sel_ram  = mem_addr[31:28] == 4'h0;
    wire sel_ecg  = mem_addr[31:28] == 4'h1;
    wire sel_io   = mem_addr[31:28] == 4'h2;
    wire sel_acc  = mem_addr[31:28] == 4'h3;
    wire access   = mem_valid && !mem_ready;
    wire wr       = |mem_wstrb;

    // program RAM (8K x 32)
    // Four 8-bit lanes so Quartus infers M9K blocks with byte writes (sb/sh)
    reg [7:0] ram0 [0:16383], ram1 [0:16383], ram2 [0:16383], ram3 [0:16383];
    reg [7:0] rq0, rq1, rq2, rq3;
    wire [31:0] ram_q = {rq3, rq2, rq1, rq0};
    wire [13:0] ra = mem_addr[15:2];
    wire ram_we = access && sel_ram && wr;
    initial begin
        $readmemh("fw/firmware_b0.hex", ram0); $readmemh("fw/firmware_b1.hex", ram1);
        $readmemh("fw/firmware_b2.hex", ram2); $readmemh("fw/firmware_b3.hex", ram3);
    end
    always @(posedge clk) begin if (ram_we && mem_wstrb[0]) ram0[ra] <= mem_wdata[ 7: 0]; rq0 <= ram0[ra]; end
    always @(posedge clk) begin if (ram_we && mem_wstrb[1]) ram1[ra] <= mem_wdata[15: 8]; rq1 <= ram1[ra]; end
    always @(posedge clk) begin if (ram_we && mem_wstrb[2]) ram2[ra] <= mem_wdata[23:16]; rq2 <= ram2[ra]; end
    always @(posedge clk) begin if (ram_we && mem_wstrb[3]) ram3[ra] <= mem_wdata[31:24]; rq3 <= ram3[ra]; end

    //  shared ECG RAM (16K x 32, true dual port)
    reg  [31:0] ecg [0:16383];
    reg  [31:0] ecg_qa, ecg_qb;
    wire [13:0] acc_ecg_addr;
    initial $readmemh("data/ecg.hex", ecg);   // paths relative to project root
    always @(posedge clk) begin                      // port A: CPU (word writes)
        if (access && sel_ecg && wr) ecg[mem_addr[15:2]] <= mem_wdata;
        ecg_qa <= ecg[mem_addr[15:2]];
    end
    always @(posedge clk) ecg_qb <= ecg[acc_ecg_addr]; // port B: accelerator

    //  accelerator
    wire [31:0] acc_q;
    pan_tompkins_accel #(.AW(14)) accel (
        .clk(clk), .rst(rst),
        .sel(access && sel_acc), .we(wr), .addr(mem_addr[12:0]), .wdata(mem_wdata), .rdata(acc_q),
        .ecg_addr(acc_ecg_addr), .ecg_q(ecg_qb), .irq_done()
    );

    //  UART + LEDs
    wire       tx_busy, rx_valid_p;
    wire [7:0] rx_byte;
    reg  [7:0] rx_data;
    reg        rx_valid;
    reg  [31:0] io_q;
    wire io_tx  = access && sel_io && wr && mem_addr[7:0] == 8'h00;
    uart_tx #(.CLKS_PER_BIT(CLKS_PER_BIT)) utx (.clk(clk), .rst(rst), .start(io_tx),
        .data(mem_wdata[7:0]), .tx(uart_tx), .busy(tx_busy));
    uart_rx #(.CLKS_PER_BIT(CLKS_PER_BIT)) urx (.clk(clk), .rst(rst), .rx_in(uart_rx),
        .data(rx_byte), .valid(rx_valid_p));

    always @(posedge clk) begin
        if (rst) begin ledr <= 0; ledg <= 0; rx_valid <= 0; end
        else begin
            if (rx_valid_p) begin rx_data <= rx_byte; rx_valid <= 1; end
            if (access && sel_io && !wr && mem_addr[7:0] == 8'h00) rx_valid <= 0;
            if (access && sel_io && wr && mem_addr[7:0] == 8'h10) ledr <= mem_wdata[17:0];
            if (access && sel_io && wr && mem_addr[7:0] == 8'h14) ledg <= mem_wdata[8:0];
        end
        case (mem_addr[7:0])
            8'h00:   io_q <= {24'd0, rx_data};
            8'h04:   io_q <= {30'd0, rx_valid, tx_busy | io_tx};
            8'h10:   io_q <= ledr;
            8'h14:   io_q <= ledg;
            default: io_q <= 0;
        endcase
    end

    // bus response (1 wait state)
    reg [3:0] sel_q;
    always @(posedge clk) begin
        mem_ready <= access;               // ready one cycle after request
        sel_q     <= {sel_acc, sel_io, sel_ecg, sel_ram};
    end
    always @(*) begin
        case (1'b1)
            sel_q[0]: mem_rdata = ram_q;
            sel_q[1]: mem_rdata = ecg_qa;
            sel_q[2]: mem_rdata = io_q;
            sel_q[3]: mem_rdata = acc_q;
            default:  mem_rdata = 32'd0;
        endcase
    end
endmodule
