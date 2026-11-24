library ieee;
use ieee.std_logic_1164.all;

entity counter is
  generic (width : integer := 8; delay : time := 1 ns);
  port (clk, rst : in std_logic;
        q : out std_logic_vector(7 downto 0));
end counter;

architecture rtl of counter is
begin
  q <= (others => '0') when rst = '1';
end rtl;
