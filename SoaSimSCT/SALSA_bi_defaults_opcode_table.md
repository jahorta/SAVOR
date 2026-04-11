# SALSA `bi_defaults` opcode parameter table

Generated from `jahorta/SALSA` `SALSA/BaseInstructions/bi_defaults.py`.

| Opcode | Param Count | SCPT Params | Loop Params | Jump Param | Notes |
|---:|---:|---|---|---:|---|
| 0 | 2 | 0:scpt|float |  | 1 | Jump |
| 1 | 0 |  |  |  |  |
| 2 | 0 |  |  |  |  |
| 3 | 4 | 0:scpt|float | 2-3 (iter param 1) | 3 | switch [2, 3]; Switch |
| 4 | 0 |  |  |  |  |
| 5 | 2 | 1:scpt|byte |  |  |  |
| 6 | 2 | 1:scpt|int |  |  |  |
| 7 | 2 | 1:scpt|float |  |  |  |
| 8 | 0 |  |  |  |  |
| 9 | 1 | 0:scpt|skip |  |  |  |
| 10 | 1 |  |  | 0 | Jump |
| 11 | 1 |  |  |  | Jump |
| 12 | 0 |  |  |  |  |
| 13 | 0 |  |  |  |  |
| 14 | 0 |  |  |  |  |
| 15 | 0 |  |  |  |  |
| 16 | 1 | 0:scpt|int |  |  |  |
| 17 | 1 | 0:scpt|int |  |  |  |
| 18 | 1 | 0:scpt|int |  |  |  |
| 19 | 1 | 0:scpt|int |  |  |  |
| 20 | 1 | 0:scpt|short |  |  |  |
| 21 | 1 | 0:scpt|short |  |  |  |
| 22 | 0 |  |  |  |  |
| 23 | 1 |  |  |  | String |
| 24 | 1 |  |  |  | String |
| 25 | 2 | 0:scpt|int |  |  | String |
| 26 | 1 | 0:scpt|int |  |  |  |
| 27 | 1 | 0:scpt|short |  |  |  |
| 28 | 4 | 0:scpt|short, 1:scpt|int, 2:scpt|int, 3:scpt|float |  |  |  |
| 29 | 4 | 0:scpt|short, 1:scpt|int, 2:scpt|int, 3:scpt|float |  |  |  |
| 30 | 2 | 0:scpt|short, 1:scpt|int |  |  |  |
| 31 | 8 | 0:scpt|short, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|int |  |  |  |
| 32 | 8 | 0:scpt|short, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|int |  |  |  |
| 33 | 8 | 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|int |  |  |  |
| 34 | 11 | 0:scpt|short, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|int, 7:scpt|int, 8:scpt|float, 9:scpt|int, 10:scpt|int |  |  |  |
| 35 | 11 | 0:scpt|short, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|int, 7:scpt|int, 8:scpt|float, 9:scpt|int, 10:scpt|int |  |  |  |
| 36 | 8 | 0:scpt|short, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|int, 5:scpt|int, 6:scpt|int, 7:scpt|float |  |  |  |
| 37 | 8 | 0:scpt|short, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|int, 5:scpt|int, 6:scpt|int, 7:scpt|float |  |  |  |
| 38 | 5 | 0:scpt|short, 1:scpt|short, 2:scpt|float, 3:scpt|float, 4:scpt|float |  |  |  |
| 39 | 2 | 0:scpt|short, 1:scpt|float |  |  |  |
| 40 | 3 | 0:scpt|short, 1:scpt|float, 2:scpt|short |  |  |  |
| 41 | 1 | 0:scpt|int |  |  |  |
| 42 | 3 | 0:scpt|int, 2:scpt|short |  |  |  |
| 43 | 1 |  |  |  | String |
| 44 | 0 |  |  |  |  |
| 45 | 0 |  |  |  |  |
| 46 | 4 | 0:scpt|float, 1:scpt|float, 2:scpt|float, 3:scpt|int |  |  |  |
| 47 | 4 | 0:scpt|float, 1:scpt|float, 2:scpt|float, 3:scpt|int |  |  |  |
| 48 | 1 | 0:scpt|int |  |  |  |
| 49 | 1 | 0:scpt|float |  |  |  |
| 50 | 6 | 0:scpt|float, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float |  |  |  |
| 51 | 4 | 0:scpt|int, 1:scpt|byte, 2:scpt|byte, 3:scpt|float |  |  |  |
| 52 | 6 | 0:scpt|float, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float |  |  |  |
| 53 | 20 | 0:scpt|short, 1:scpt|int, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|int, 6:scpt|int, 7:scpt|float, 8:scpt|int, 9:scpt|float, 10:scpt|int, 11:scpt|int, 12:scpt|float, 13:scpt|float, 14:scpt|float, 15:scpt|float, 16:scpt|int, 17:scpt|int, 18:scpt|float, 19:scpt|int |  |  |  |
| 54 | 2 | 0:scpt|int |  |  | String |
| 55 | 3 | 0:scpt|byte, 1:scpt|byte, 2:scpt|int |  |  |  |
| 56 | 1 | 0:scpt|short |  |  |  |
| 57 | 0 |  |  |  |  |
| 58 | 0 |  |  |  |  |
| 59 | 1 | 0:scpt|int |  |  |  |
| 60 | 1 | 0:scpt|int |  |  |  |
| 61 | 3 | 0:scpt|int, 1:scpt|int, 2:scpt|int |  |  |  |
| 62 | 2 | 0:scpt|int, 1:scpt|int |  |  |  |
| 63 | 2 | 0:scpt|int, 1:scpt|int |  |  |  |
| 64 | 1 | 0:scpt|int |  |  |  |
| 65 | 1 | 0:scpt|int |  |  |  |
| 66 | 3 | 0:scpt|int, 1:scpt|byte, 2:scpt|byte |  |  |  |
| 67 | 0 |  |  |  |  |
| 68 | 0 |  |  |  |  |
| 69 | 1 |  |  |  | String |
| 70 | 4 | 0:scpt|float, 1:scpt|float, 2:scpt|float, 3:scpt|int |  |  |  |
| 71 | 4 | 0:scpt|float, 1:scpt|float, 2:scpt|float, 3:scpt|int |  |  |  |
| 72 | 9 | 0:scpt|float, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|float, 8:scpt|int |  |  |  |
| 73 | 10 | 0:scpt|float, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|float, 8:scpt|int, 9:scpt|short |  |  |  |
| 74 | 7 | 0:scpt|float, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|int |  |  |  |
| 75 | 12 | 0:scpt|float, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|float, 8:scpt|int, 9:scpt|float, 10:scpt|float, 11:scpt|int |  |  |  |
| 76 | 1 | 0:scpt|int |  |  |  |
| 77 | 5 | 0:scpt|int, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float |  |  |  |
| 78 | 4 | 0:scpt|short, 1:scpt|int, 2:scpt|int, 3:scpt|float |  |  |  |
| 79 | 7 | 0:scpt|int, 1:scpt|int, 2:scpt|int, 3:scpt|int, 4:scpt|float, 5:scpt|float, 6:scpt|int |  |  |  |
| 80 | 0 |  |  |  |  |
| 81 | 9 | 0:scpt|int, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|float, 8:scpt|float |  |  |  |
| 82 | 4 | 0:scpt|int, 1:scpt|float, 2:scpt|float, 3:scpt|float |  |  |  |
| 83 | 7 | 0:scpt|int, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float |  |  |  |
| 84 | 10 | 0:scpt|int, 1:scpt|int, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|float, 8:scpt|float, 9:scpt|float |  |  |  |
| 85 | 3 | 0:scpt|int, 2:scpt|short |  |  |  |
| 86 | 3 | 0:scpt|int, 2:scpt|short |  |  |  |
| 87 | 3 | 0:scpt|int, 2:scpt|short |  |  |  |
| 88 | 3 | 0:scpt|int, 2:scpt|short |  |  |  |
| 89 | 11 | 0:scpt|short, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|int, 7:scpt|int, 8:scpt|float, 9:scpt|int, 10:scpt|int |  |  |  |
| 90 | 11 | 0:scpt|short, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|int, 7:scpt|int, 8:scpt|float, 9:scpt|int, 10:scpt|int |  |  |  |
| 91 | 2 | 0:scpt|int, 1:scpt|int |  |  |  |
| 92 | 1 | 0:scpt|int |  |  |  |
| 93 | 1 | 0:scpt|int |  |  |  |
| 94 | 1 | 0:scpt|int |  |  |  |
| 95 | 7 | 0:scpt|float, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|int |  |  |  |
| 96 | 7 | 0:scpt|float, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|int |  |  |  |
| 97 | 3 | 0:scpt|short, 1:scpt|int, 2:scpt|int |  |  |  |
| 98 | 2 | 1:scpt|short |  |  |  |
| 99 | 2 | 1:scpt|short |  |  |  |
| 100 | 3 | 0:scpt|short, 1:scpt|int, 2:scpt|float |  |  |  |
| 101 | 1 | 0:scpt|int |  |  |  |
| 102 | 1 | 0:scpt|int |  |  |  |
| 103 | 0 |  |  |  |  |
| 104 | 3 | 0:scpt|int, 1:scpt|float, 2:scpt|int |  |  |  |
| 105 | 1 | 0:scpt|int |  |  |  |
| 106 | 9 | 0:scpt|short, 1:scpt|int, 2:scpt|short, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|float, 8:scpt|float |  |  |  |
| 107 | 2 | 0:scpt|short, 1:scpt|short |  |  |  |
| 108 | 11 | 0:scpt|short, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|int, 7:scpt|int, 8:scpt|float, 9:scpt|int, 10:scpt|int |  |  |  |
| 109 | 9 | 0:scpt|byte, 1:scpt|byte, 2:scpt|byte, 3:scpt|int, 4:scpt|float, 5:scpt|float, 6:scpt|int, 7:scpt|float, 8:scpt|float |  |  |  |
| 110 | 1 |  |  |  | String |
| 111 | 2 | 0:scpt|int, 1:scpt|int |  |  |  |
| 112 | 4 | 0:scpt|int, 1:scpt|int, 2:scpt|int, 3:scpt|int |  |  |  |
| 113 | 1 |  |  |  | String |
| 114 | 2 | 0:scpt|int, 1:scpt|short |  |  |  |
| 115 | 0 |  |  |  |  |
| 116 | 7 | 0:scpt|int, 1:scpt|byte, 2:scpt|byte, 3:scpt|int, 4:scpt|float, 5:scpt|float, 6:scpt|float |  |  |  |
| 117 | 2 | 0:scpt|short, 1:scpt|int |  |  |  |
| 118 | 3 | 0:scpt|int, 2:scpt|short |  |  |  |
| 119 | 3 | 0:scpt|int, 2:scpt|short |  |  |  |
| 120 | 7 | 0:scpt|short, 1:scpt|int, 2:scpt|int, 3:scpt|float, 4:scpt|float, 5:scpt|int, 6:scpt|int |  |  |  |
| 121 | 8 | 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|int |  |  |  |
| 122 | 5 | 0:scpt|float, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|int |  |  |  |
| 123 | 2 | 0:scpt|float, 1:scpt|int |  |  |  |
| 124 | 2 | 0:scpt|float, 1:scpt|int |  |  |  |
| 125 | 0 |  |  |  |  |
| 126 | 2 | 1:scpt|int |  |  |  |
| 127 | 2 | 1:scpt|int |  |  |  |
| 128 | 6 | 0:scpt|short, 1:scpt|int, 2:scpt|int, 3:scpt|float, 4:scpt|float, 5:scpt|int |  |  |  |
| 129 | 2 | 0:scpt|int |  |  |  |
| 130 | 9 | 0:scpt|short, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|float, 8:scpt|float |  |  |  |
| 131 | 5 | 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|int |  |  |  |
| 132 | 8 | 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|int |  |  |  |
| 133 | 11 | 1:scpt|int, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|float, 8:scpt|float, 9:scpt|float, 10:scpt|int |  |  |  |
| 134 | 7 | 0:scpt|short, 1:scpt|int, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|int, 6:scpt|int |  |  |  |
| 135 | 3 | 0:scpt|int, 2:scpt|short |  |  |  |
| 136 | 5 | 0:scpt|short, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|int |  |  |  |
| 137 | 1 | 0:scpt|int |  |  |  |
| 138 | 0 |  |  |  |  |
| 139 | 8 | 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|int |  |  |  |
| 140 | 1 | 0:scpt|int |  |  |  |
| 141 | 1 | 0:scpt|int |  |  |  |
| 142 | 5 | 0:scpt|byte, 1:scpt|byte, 2:scpt|byte, 3:scpt|float, 4:scpt|int |  |  |  |
| 143 | 0 |  |  |  |  |
| 144 | 2 | 1:scpt|int |  |  | String |
| 145 | 1 | 0:scpt|int |  |  |  |
| 146 | 1 | 0:scpt|int |  |  |  |
| 147 | 10 | 0:scpt|int, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|byte, 5:scpt|float, 6:scpt|int, 7:scpt|byte, 8:scpt|byte, 9:scpt|int |  |  |  |
| 148 | 10 | 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|float, 8:scpt|float, 9:scpt|int |  |  |  |
| 149 | 1 | 0:scpt|int |  |  |  |
| 150 | 1 | 0:scpt|int |  |  |  |
| 151 | 2 | 0:scpt|int, 1:scpt|int |  |  |  |
| 152 | 2 | 1:scpt|int |  |  |  |
| 153 | 4 | 1:scpt|byte, 2:scpt|byte, 3:scpt|short |  |  |  |
| 154 | 1 | 0:scpt|int |  |  |  |
| 155 | 3 | 0:scpt|int, 2:scpt|int |  |  | String |
| 156 | 0 |  |  |  |  |
| 157 | 1 | 0:scpt|int |  |  |  |
| 158 | 1 | 0:scpt|int |  |  |  |
| 159 | 9 | 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|int, 8:scpt|byte |  |  |  |
| 160 | 7 | 0:scpt|int, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float |  |  |  |
| 161 | 11 | 0:scpt|short, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|float, 8:scpt|float, 9:scpt|int, 10:scpt|int |  |  |  |
| 162 | 1 | 0:scpt|int |  |  |  |
| 163 | 9 | 0:scpt|int, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|float, 8:scpt|int |  |  |  |
| 164 | 2 | 0:scpt|short, 1:scpt|int |  |  |  |
| 165 | 19 | 0:scpt|int, 1:scpt|int, 2:scpt|int, 3:scpt|int, 4:scpt|int, 5:scpt|int, 6:scpt|int, 7:scpt|int, 8:scpt|int, 9:scpt|int, 10:scpt|int, 11:scpt|int, 12:scpt|int, 13:scpt|float, 14:scpt|float, 15:scpt|float, 16:scpt|float, 17:scpt|int, 18:scpt|int |  |  |  |
| 166 | 2 | 0:scpt|int, 1:scpt|int |  |  |  |
| 167 | 0 |  |  |  |  |
| 168 | 1 | 0:scpt|int |  |  |  |
| 169 | 4 | 0:scpt|float, 1:scpt|float, 2:scpt|float, 3:scpt|float |  |  |  |
| 170 | 0 |  |  |  |  |
| 171 | 0 |  |  |  |  |
| 172 | 1 | 0:scpt|int |  |  |  |
| 173 | 1 | 0:scpt|int |  |  |  |
| 174 | 1 | 0:scpt|int |  |  |  |
| 175 | 2 | 0:scpt|short, 1:scpt|float |  |  |  |
| 176 | 6 | 0:scpt|short, 1:scpt|short, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|int |  |  |  |
| 177 | 13 | 0:scpt|int, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|float, 8:scpt|float, 9:scpt|float, 10:scpt|float, 11:scpt|float, 12:scpt|float |  |  |  |
| 178 | 8 | 0:scpt|float, 1:scpt|float, 2:scpt|float, 3:scpt|int, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|int |  |  |  |
| 179 | 4 | 0:scpt|float, 1:scpt|float, 2:scpt|float, 3:scpt|int |  |  |  |
| 180 | 1 | 0:scpt|int |  |  |  |
| 181 | 1 | 0:scpt|int |  |  |  |
| 182 | 0 |  |  |  |  |
| 183 | 1 | 0:scpt|short |  |  |  |
| 184 | 2 | 0:scpt|byte, 1:scpt|int |  |  |  |
| 185 | 1 | 0:scpt|int |  |  |  |
| 186 | 1 | 0:scpt|int |  |  |  |
| 187 | 1 | 0:scpt|int |  |  |  |
| 188 | 1 | 0:scpt|int |  |  |  |
| 189 | 0 |  |  |  |  |
| 190 | 3 | 0:scpt|short, 1:scpt|int, 2:scpt|float |  |  |  |
| 191 | 1 | 0:scpt|int |  |  |  |
| 192 | 1 | 0:scpt|int |  |  |  |
| 193 | 1 | 0:scpt|int |  |  |  |
| 194 | 2 | 0:scpt|int, 1:scpt|int |  |  |  |
| 195 | 2 | 0:scpt|int, 1:scpt|int |  |  |  |
| 196 | 0 |  |  |  |  |
| 197 | 2 | 0:scpt|int, 1:scpt|int |  |  |  |
| 198 | 2 | 0:scpt|short, 1:scpt|float |  |  |  |
| 199 | 1 | 0:scpt|short |  |  |  |
| 200 | 0 |  |  |  |  |
| 201 | 0 |  |  |  |  |
| 202 | 0 |  |  |  |  |
| 203 | 1 | 0:scpt|int |  |  |  |
| 204 | 2 | 0:scpt|int, 1:scpt|int |  |  |  |
| 205 | 1 | 0:scpt|int |  |  |  |
| 206 | 0 |  |  |  |  |
| 207 | 3 | 0:scpt|int, 1:scpt|byte, 2:scpt|short |  |  |  |
| 208 | 0 |  |  |  |  |
| 209 | 0 |  |  |  |  |
| 210 | 1 |  |  |  | String |
| 211 | 0 |  |  |  |  |
| 212 | 5 | 0:scpt|int, 1:scpt|int, 2:scpt|int, 3:scpt|int, 4:scpt|int |  |  |  |
| 213 | 17 | 0:scpt|int, 1:scpt|byte, 2:scpt|byte, 3:scpt|byte, 4:scpt|byte, 5:scpt|byte, 6:scpt|byte, 7:scpt|byte, 8:scpt|byte, 9:scpt|byte, 10:scpt|byte, 11:scpt|byte, 12:scpt|byte, 13:scpt|byte, 14:scpt|byte, 15:scpt|byte, 16:scpt|byte |  |  |  |
| 214 | 1 |  |  |  | String |
| 215 | 2 | 0:scpt|int |  |  | String |
| 216 | 1 | 0:scpt|int |  |  |  |
| 217 | 1 | 0:scpt|int |  |  |  |
| 218 | 1 | 0:scpt|int |  |  |  |
| 219 | 1 | 0:scpt|int |  |  |  |
| 220 | 9 | 0:scpt|short, 1:scpt|int, 2:scpt|short, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|float, 8:scpt|float |  |  |  |
| 221 | 2 | 0:scpt|float, 1:scpt|int |  |  |  |
| 222 | 1 | 0:scpt|int |  |  |  |
| 223 | 25 | 0:scpt|int, 1:scpt|int, 2:scpt|int, 3:scpt|int, 4:scpt|int, 5:scpt|int, 6:scpt|int, 7:scpt|int, 8:scpt|int, 9:scpt|int, 10:scpt|int, 11:scpt|int, 12:scpt|int, 13:scpt|int, 14:scpt|int, 15:scpt|int, 16:scpt|int, 17:scpt|int, 18:scpt|int, 19:scpt|int, 20:scpt|int, 21:scpt|int, 22:scpt|int, 23:scpt|int, 24:scpt|int |  |  |  |
| 224 | 25 | 0:scpt|int, 1:scpt|int, 2:scpt|int, 3:scpt|int, 4:scpt|int, 5:scpt|float, 6:scpt|float, 7:scpt|float, 8:scpt|float, 9:scpt|float, 10:scpt|float, 11:scpt|float, 12:scpt|float, 13:scpt|float, 14:scpt|float, 15:scpt|float, 16:scpt|float, 17:scpt|float, 18:scpt|float, 19:scpt|float, 20:scpt|float, 21:scpt|float, 22:scpt|float, 23:scpt|float, 24:scpt|float |  |  |  |
| 225 | 7 | 0:scpt|int, 1:scpt|int, 2:scpt|int, 3:scpt|int, 4:scpt|int, 5:scpt|int, 6:scpt|int |  |  |  |
| 226 | 2 | 0:scpt|int, 1:scpt|int |  |  |  |
| 227 | 1 | 0:scpt|int |  |  |  |
| 228 | 0 |  |  |  |  |
| 229 | 0 |  |  |  |  |
| 230 | 7 | 0:scpt|float, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|int |  |  |  |
| 231 | 1 | 0:scpt|int |  |  |  |
| 232 | 14 | 0:scpt|int, 1:scpt|int, 2:scpt|int, 3:scpt|int, 4:scpt|int, 5:scpt|int, 6:scpt|int, 7:scpt|int, 8:scpt|int, 9:scpt|int, 10:scpt|int, 11:scpt|int, 12:scpt|int, 13:scpt|int |  |  |  |
| 233 | 2 | 0:scpt|int, 1:scpt|int |  |  |  |
| 234 | 0 |  |  |  |  |
| 235 | 1 | 0:scpt|int |  |  |  |
| 236 | 6 | 0:scpt|float, 1:scpt|float, 2:scpt|float, 3:scpt|int, 4:scpt|int, 5:scpt|int |  |  |  |
| 237 | 0 |  |  |  |  |
| 238 | 3 | 0:scpt|float, 1:scpt|float, 2:scpt|float |  |  |  |
| 239 | 1 | 0:scpt|int |  |  |  |
| 240 | 6 | 0:scpt|byte, 1:scpt|int, 2:scpt|float, 3:scpt|float, 4:scpt|int, 5:scpt|int |  |  |  |
| 241 | 0 |  |  |  |  |
| 242 | 3 | 0:scpt|int, 1:scpt|byte, 2:scpt|int |  |  |  |
| 243 | 10 | 0:scpt|int, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|byte, 5:scpt|float, 6:scpt|int, 7:scpt|byte, 8:scpt|byte, 9:scpt|int |  |  |  |
| 244 | 2 | 0:scpt|int, 1:scpt|int |  |  |  |
| 245 | 2 | 1:scpt|short |  |  |  |
| 246 | 2 | 1:scpt|short |  |  |  |
| 247 | 0 |  |  |  |  |
| 248 | 1 |  |  |  | String |
| 249 | 2 | 0:scpt|short, 1:scpt|int |  |  |  |
| 250 | 1 |  |  |  | String |
| 251 | 2 | 0:scpt|int, 1:scpt|float |  |  |  |
| 252 | 2 | 0:scpt|int, 1:scpt|float |  |  |  |
| 253 | 2 | 0:scpt|int, 1:scpt|int |  |  |  |
| 254 | 0 |  |  |  |  |
| 255 | 0 |  |  |  |  |
| 256 | 1 | 0:scpt|int |  |  |  |
| 257 | 1 |  |  |  | String |
| 258 | 2 | 1:scpt|int |  |  |  |
| 259 | 1 | 0:scpt|int |  |  |  |
| 260 | 0 |  |  |  |  |
| 261 | 0 |  |  |  |  |
| 262 | 5 | 0:scpt|short, 1:scpt|short, 2:scpt|short, 3:scpt|float, 4:scpt|int |  |  |  |
| 263 | 2 | 1:scpt|float |  |  |  |
| 264 | 8 | 0:scpt|short, 1:scpt|float, 2:scpt|float, 3:scpt|float, 4:scpt|float, 5:scpt|float, 6:scpt|float, 7:scpt|int |  |  |  |
| 265 | 2 | 0:scpt|int |  |  | String |