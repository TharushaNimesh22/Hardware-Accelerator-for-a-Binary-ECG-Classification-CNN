module pan_tompkins_accel #(
    parameter AW      = 14,    // ECG RAM address width (words)
    parameter LEARN   = 400,
    parameter REFRACT = 40,
    parameter DELAY   = 38,
    parameter PK_AW   = 9      // 512 result entries
)(
    input             clk,
    input             rst,
    // CPU register interface (1-cycle read latency)
    input             sel,
    input             we,
    input      [12:0] addr,     // byte address inside 8 KB window
    input      [31:0] wdata,
    output     [31:0] rdata,
    // ECG memory read port
    output reg [AW-1:0] ecg_addr,
    input      [31:0]   ecg_q,
    output              irq_done
);
    //  regs
    reg [AW-1:0] src_reg;
    reg [15:0]   len_reg;
    reg          busy, done, ovf;
    reg [31:0]   cycles;
    reg [PK_AW:0] npeaks;

    // fetch
    reg [15:0] fetch_cnt;
    reg        fetching;
    reg        v0;              // ecg_q valid this cycle
    wire       start = sel & we & (addr[12:2] == 11'd0) & wdata[0] & ~busy;

    //  pipeline storage
    reg signed [31:0] xh   [1:12];   // input history
    reg signed [31:0] y1_d1, y1_d2;
    reg signed [31:0] lpf;  reg v1;
    reg signed [31:0] uh   [1:32];   // LPF history
    reg signed [31:0] s;
    reg signed [31:0] hpf;  reg v2;
    reg signed [31:0] wh   [1:4];    // HPF history
    reg signed [31:0] der;  reg v3;
    reg signed [31:0] sq;   reg v4;
    reg signed [31:0] qh   [1:32];   // squared history
    reg signed [31:0] qsum;
    reg signed [31:0] mwi;  reg v5;

    // peak detector
    reg [15:0]        n;
    reg signed [31:0] learn_max, spki, npki, cur_max, prev;
    reg [15:0]        cur_idx, last_idx;
    reg               rising, have_last;
    reg [15:0]        rr_avg;  reg rr_valid;           // running RR average (samples)
    reg signed [31:0] cand_amp; reg [15:0] cand_idx; reg cand_valid;   // best noise peak
    reg               pend;    reg signed [31:0] pk_p; reg [15:0] pki_p; // peak awaiting decision

    integer i;

    // combinational datapath
    wire signed [31:0] x_in = {{16{ecg_q[15]}}, ecg_q[15:0]};
    wire signed [31:0] y1   = (y1_d1 <<< 1) - y1_d2 + x_in - (xh[6] <<< 1) + xh[12];
    wire signed [31:0] s_n  = s + lpf - uh[32];
    wire signed [31:0] hp_n = ((uh[16] <<< 5) - s_n) >>> 5;   // uh[16] = u[n-16]
    wire signed [31:0] d_raw= ((hpf <<< 1) + wh[1] - wh[3] - (wh[4] <<< 1)) >>> 3;
    wire signed [31:0] d_sh = d_raw >>> 2;
    wire signed [31:0] d_n  = (d_sh >  32'sd16383) ?  32'sd16383 :
                              (d_sh < -32'sd16383) ? -32'sd16383 : d_sh;
    wire signed [15:0] der16 = der[15:0];                       // |der| <= 16383
    wire signed [31:0] q_n  = (der16 * der16) >>> 3;
    wire signed [31:0] qs_n = qsum + sq - qh[32];
    wire signed [31:0] m    = mwi;

    // peak detector combinational
    wire signed [31:0] lm_n   = (m > learn_max) ? m : learn_max;
    wire signed [31:0] thr    = npki + ((spki - npki) >>> 2);
    wire        [15:0] rr_lim = rr_avg + (rr_avg >> 1) + (rr_avg >> 3);   // ~1.66 x RR avg
    wire               sb     = have_last && rr_valid && cand_valid && (n - last_idx > rr_lim) &&
                                (cand_amp > (thr >>> 1)) && (cand_idx - last_idx > REFRACT);
    wire               p_sig  = !sb && pend && (pk_p > thr);
    wire               p_qrs  = p_sig && (!have_last || (pki_p - last_idx > REFRACT));
    wire               p_noi  = !sb && pend && !(pk_p > thr);
    wire               accept = sb || p_qrs;
    wire        [15:0] acc_idx = sb ? cand_idx : pki_p;
    wire signed [31:0] acc_amp = sb ? cand_amp : pk_p;
    wire signed [31:0] spki_n  = sb ? spki + ((acc_amp - spki) >>> 2) : spki + ((acc_amp - spki) >>> 3);
    wire        [15:0] rr      = acc_idx - last_idx;
    wire signed [17:0] rr_diff = $signed({2'b00, rr}) - $signed({2'b00, rr_avg});
    wire signed [17:0] rr_step = rr_diff >>> 3;                         // (rr - avg) / 8, floor
    wire               fall   = rising && !(m > cur_max) && (m < cur_max - (cur_max >>> 2));

    // result RAM (written by detector, read by CPU)
    reg [15:0] pk_ram [0:(1<<PK_AW)-1];
    reg [15:0] pk_q;
    wire       pk_we = v5 && accept && !npeaks[PK_AW];

    always @(posedge clk) begin
        if (pk_we) pk_ram[npeaks[PK_AW-1:0]] <= acc_idx - DELAY;
        pk_q <= pk_ram[addr[PK_AW+1:2]];
    end

    //  control + pipeline
    always @(posedge clk) begin
        if (rst) begin
            busy <= 0; done <= 0; ovf <= 0; src_reg <= 1; len_reg <= 0;
            fetching <= 0; v0 <= 0; v1 <= 0; v2 <= 0; v3 <= 0; v4 <= 0; v5 <= 0;
            cycles <= 0; npeaks <= 0;
        end else begin
            // register writes
            if (sel && we && addr[12:2] == 11'd2) src_reg <= wdata[AW-1:0];
            if (sel && we && addr[12:2] == 11'd3) len_reg <= wdata[15:0];

            // start: clear all state
            if (start) begin
                busy <= 1; done <= 0; ovf <= 0; cycles <= 0; npeaks <= 0;
                fetching <= (len_reg != 0); fetch_cnt <= 0; ecg_addr <= src_reg;
                v0 <= 0; v1 <= 0; v2 <= 0; v3 <= 0; v4 <= 0; v5 <= 0;
                for (i = 1; i <= 12; i = i + 1) xh[i] <= 0;
                for (i = 1; i <= 32; i = i + 1) begin uh[i] <= 0; qh[i] <= 0; end
                for (i = 1; i <= 4;  i = i + 1) wh[i] <= 0;
                y1_d1 <= 0; y1_d2 <= 0; s <= 0; qsum <= 0;
                n <= 0; learn_max <= 0; spki <= 0; npki <= 0; cur_max <= 0; prev <= 0;
                cur_idx <= 0; last_idx <= 0; rising <= 0; have_last <= 0;
                rr_avg <= 0; rr_valid <= 0; cand_amp <= 0; cand_idx <= 0; cand_valid <= 0;
                pend <= 0; pk_p <= 0; pki_p <= 0;
            end else if (busy) begin
                cycles <= cycles + 1;

                //  stage 0: fetch (1 read/clk, data next clk)
                v0 <= fetching;
                if (fetching) begin
                    ecg_addr  <= ecg_addr + 1'b1;
                    fetch_cnt <= fetch_cnt + 1'b1;
                    if (fetch_cnt == len_reg - 1) fetching <= 0;
                end

                //  stage 1: low-pass
                v1 <= v0;
                if (v0) begin
                    xh[1] <= x_in;
                    for (i = 2; i <= 12; i = i + 1) xh[i] <= xh[i-1];
                    y1_d2 <= y1_d1; y1_d1 <= y1; lpf <= y1;
                end
                //  stage 2: high-pass (all-pass delay minus 32-pt moving sum)
                v2 <= v1;
                if (v1) begin
                    uh[1] <= lpf;
                    for (i = 2; i <= 32; i = i + 1) uh[i] <= uh[i-1];
                    s <= s_n; hpf <= hp_n;
                end
                //  stage 3: derivative + scaling
                v3 <= v2;
                if (v2) begin
                    wh[1] <= hpf;
                    for (i = 2; i <= 4; i = i + 1) wh[i] <= wh[i-1];
                    der <= d_n;
                end
                // stage 4: squaring
                v4 <= v3;
                if (v3) sq <= q_n;
                //  stage 5: moving-window integration
                v5 <= v4;
                if (v4) begin
                    qh[1] <= sq;
                    for (i = 2; i <= 32; i = i + 1) qh[i] <= qh[i-1];
                    qsum <= qs_n; mwi <= qs_n >>> 5;
                end
                //  stage 6: adaptive threshold peak detection
                if (v5) begin
                    n <= n + 1'b1;
                    if (n < LEARN) begin
                        learn_max <= lm_n;
                        if (n == LEARN - 1) begin spki <= lm_n >>> 1; npki <= lm_n >>> 3; end
                    end
                    // decision on pending peak / searchback (one accept per sample max)
                    if (!sb && pend) pend <= 0;
                    if (p_noi) begin
                        npki <= npki + ((pk_p - npki) >>> 3);
                        if (!cand_valid || pk_p > cand_amp) begin
                            cand_amp <= pk_p; cand_idx <= pki_p; cand_valid <= 1;
                        end
                    end
                    if (accept) begin
                        if (have_last) begin
                            rr_avg   <= rr_valid ? rr_avg + rr_step[15:0] : rr;
                            rr_valid <= 1;
                        end
                        last_idx <= acc_idx; have_last <= 1; spki <= spki_n; cand_valid <= 0;
                        if (npeaks[PK_AW]) ovf <= 1; else npeaks <= npeaks + 1'b1;
                    end
                    // local-maximum tracking on the MWI signal
                    if (rising) begin
                        if (m > cur_max) begin cur_max <= m; cur_idx <= n; end
                        else if (fall) begin
                            rising <= 0;
                            if (n >= LEARN) begin pend <= 1; pk_p <= cur_max; pki_p <= cur_idx; end
                        end
                    end else if (m > prev) begin
                        rising <= 1; cur_max <= m; cur_idx <= n;
                    end
                    prev <= m;
                    if (n == len_reg - 1) begin busy <= 0; done <= 1; end
                end
            end
        end
    end

    //  CPU read mux
    reg [31:0] reg_q;
    reg        win_q;           // 1 = last access was to the PEAK[] window
    always @(posedge clk) begin
        win_q <= addr[12];
        case (addr[12:2])
            11'd1: reg_q <= {29'd0, ovf, done, busy};
            11'd2: reg_q <= src_reg;
            11'd3: reg_q <= len_reg;
            11'd4: reg_q <= npeaks;
            11'd5: reg_q <= cycles;
            11'd6: reg_q <= spki;
            11'd7: reg_q <= npki;
            default: reg_q <= 32'd0;
        endcase
    end
    assign rdata = win_q ? {16'd0, pk_q} : reg_q;
    assign irq_done = done;
endmodule
