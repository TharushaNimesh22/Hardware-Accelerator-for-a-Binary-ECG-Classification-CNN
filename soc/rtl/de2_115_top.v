// DE2-115 top level: 50 MHz clock, KEY0 = reset, RS-232 UART to PC, LEDs
module de2_115_top (
    input         CLOCK_50,
    input  [3:0]  KEY,          // active low push buttons
    input         UART_RXD,     // PC -> FPGA
    output        UART_TXD,     // FPGA -> PC
    output [17:0] LEDR,
    output [8:0]  LEDG
);
    // synchronous reset from KEY0 (held for a few cycles after release)
    reg [3:0] rst_sh = 4'hF;
    always @(posedge CLOCK_50) rst_sh <= {rst_sh[2:0], ~KEY[0]};
    wire rst = |rst_sh;

    soc #(.CLKS_PER_BIT(434)) u_soc (       // 50e6 / 115200 = 434
        .clk(CLOCK_50), .rst(rst),
        .uart_tx(UART_TXD), .uart_rx(UART_RXD),
        .ledr(LEDR), .ledg(LEDG)
    );
endmodule
