`timescale 1ns/1ps
module support_fixture;
  typedef struct packed { logic [3:0] tag; logic [7:0] payload; } packet_t;
  typedef struct { logic [7:0] a; logic [7:0] b; } pair_t;
  packet_t packet;
  pair_t pair_value;
  logic [7:0] matrix [1:0][2:1];
  bit [7:0] two_state;
  logic delayed;
  real analog;
  string text_value;
  initial begin
    $fsdbDumpfile("support.fsdb");
    $fsdbDumpvars(0, support_fixture, "+all");
    $fsdbDumpMDA();
    packet = '{4'ha, 8'h12}; pair_value = '{8'h34, 8'h56};
    matrix[1][2] = 8'h78; matrix[1][1] = 8'h9a;
    matrix[0][2] = 8'hbc; matrix[0][1] = 8'hde;
    two_state = 8'h01; delayed = 0; analog = 1.25; text_value = "initial";
    #1; packet.payload = 8'h13; pair_value.a = 8'h35;
    matrix[1][2] = 8'h79; two_state = 8'h02; analog = -2.5; text_value = "changed";
    #1; $fsdbDumpoff(); delayed = 1;
    #1; delayed = 0;
    #1; $fsdbDumpon();
    #1; delayed = 1;
    #1; $finish;
  end
endmodule
