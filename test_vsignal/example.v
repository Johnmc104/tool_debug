// test_vsignal/example.v — Simple test design for vsignal
// Provides hierarchy with drivers, loads, registers, and connections.

module sub_mod (
    input  wire       clk,
    input  wire       rst_n,
    input  wire [7:0] data_in,
    output reg  [7:0] data_out,
    output wire       valid
);
    reg [7:0] data_reg;
    reg       valid_reg;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            data_reg  <= 8'h0;
            valid_reg <= 1'b0;
        end else begin
            data_reg  <= data_in;
            valid_reg <= |data_in;
        end
    end

    assign data_out = data_reg;
    assign valid    = valid_reg;
endmodule

module top (
    input  wire       clk,
    input  wire       rst_n,
    input  wire [7:0] data_in,
    output wire [7:0] data_out,
    output wire       valid
);
    wire [7:0] internal_data;
    wire       internal_valid;

    sub_mod u_sub (
        .clk      (clk),
        .rst_n    (rst_n),
        .data_in  (data_in),
        .data_out (internal_data),
        .valid    (internal_valid)
    );

    assign data_out = internal_data;
    assign valid    = internal_valid;
endmodule
