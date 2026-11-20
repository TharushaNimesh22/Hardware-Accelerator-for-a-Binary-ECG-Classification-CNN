// 8N1 UART transmitter
module uart_tx #(parameter CLKS_PER_BIT = 434)(   // 50 MHz / 115200
    input        clk, rst,
    input        start,
    input  [7:0] data,
    output reg   tx,
    output       busy
);
    reg [9:0]  sh;
    reg [3:0]  bits;
    reg [15:0] cnt;
    assign busy = (bits != 0);
    always @(posedge clk) begin
        if (rst) begin tx <= 1; bits <= 0; cnt <= 0; end
        else if (bits == 0) begin
            tx <= 1;
            if (start) begin sh <= {1'b1, data, 1'b0}; bits <= 10; cnt <= 0; end
        end else begin
            tx <= sh[0];
            if (cnt == CLKS_PER_BIT - 1) begin
                cnt <= 0; sh <= {1'b1, sh[9:1]}; bits <= bits - 1'b1;
            end else cnt <= cnt + 1'b1;
        end
    end
endmodule
