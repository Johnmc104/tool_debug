`timescale 1ns/1ps
module fixture_child(input wire [7:0] data);
  wire [7:0] alias_data = data;
endmodule
module fixture;
  reg clk = 0;
  reg stable = 1;
  reg [7:0] data;
  reg [0:7] ascending;
  reg [129:0] wide;
  reg [3:0] mixed;
  reg [7:0] memory [0:1];
  reg \escaped.name ;
  integer signed_number;
  real analog;
  string message;
  fixture_child child(data);
  initial begin
    $fsdbDumpfile("fixture.fsdb");
    $fsdbDumpvars(0, fixture, "+all");
    $fsdbDumpMDA();
    data = 8'h00; ascending = 8'h12; wide = 130'h1; mixed = 4'bx0z1;
    memory[0] = 8'h01; memory[1] = 8'h02;
    \escaped.name = 0;
    signed_number = -1; analog = 1.25; message = "initial";
    #1; clk = 1; data = 8'haf; ascending = 8'hb3; wide = {130{1'b1}};
    mixed = 4'bzzzz; memory[1] = 8'hcd; \escaped.name = 1;
    #1; clk = 0; data = 8'hzz; wide = {130{1'bz}}; mixed = 4'bxxxx;
    #1; clk = 1; data = 8'bx01z1010; wide = 130'h123456789abcdef; mixed = 4'bz001;
    signed_number = -7; analog = -2.5; message = "changed";
    #1; clk = 0; data = 8'hfe; ascending = 8'h01; mixed = 4'bx001;
    #0; clk = 1; data = 8'h01;
    #0; clk = 0; data = 8'h02;
    #1; $finish;
  end
endmodule
