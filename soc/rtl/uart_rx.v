// 8N1 UART receiver (used for simple PC -> board commands)
module uart_rx #(parameter CLKS_PER_BIT = 434)(
    input            clk, rst,
    input            rx_in,
    output reg [7:0] data,
    output reg       valid        // 1-clock pulse
);
    reg [1:0]  sync;
    reg [15:0] cnt;
    reg [3:0]  bitn;
    reg        active;
    wire rx = sync[1];
    always @(posedge clk) begin
        sync <= {sync[0], rx_in};
        valid <= 0;
        if (rst) begin active <= 0; cnt <= 0; sync <= 2'b11; end
        else if (!active) begin
            if (!rx) begin active <= 1; cnt <= CLKS_PER_BIT/2; bitn <= 0; end
        end else if (cnt == CLKS_PER_BIT - 1) begin
            cnt <= 0;
            bitn <= bitn + 1'b1;
            if (bitn == 0) begin if (rx) active <= 0; end           // false start
            else if (bitn <= 8) data <= {rx, data[7:1]};
            else begin active <= 0; valid <= rx; end                // stop bit
        end else cnt <= cnt + 1'b1;
    end
endmodule
