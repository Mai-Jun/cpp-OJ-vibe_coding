-- seed.sql — 5 道种子题（problems + test_cases 表），难度 2 easy / 2 medium / 1 hard。
-- 用法：schema 初始化后执行：mysql -u oj -p oj < scripts/seed.sql
-- 注意：id 显式指定以保持用例关联清晰；若 problems 已存在数据需先清空（DELETE FROM test_cases; DELETE FROM problems;）。

USE oj;

-- ============================================================
-- 1. A+B Problem（easy）
-- ============================================================
INSERT INTO problems (id, title, description, difficulty, tags, time_limit_ms, memory_limit_mb, judge_type) VALUES
(1, 'A+B Problem', '输入两个整数 a 和 b，输出它们的和 a+b。\n\n## 输入\n\n一行两个整数 a、b（-10^9 ≤ a,b ≤ 10^9），用空格分隔。\n\n## 输出\n\n一个整数，表示 a+b。\n\n## 示例\n\n输入：\n```\n1 2\n```\n输出：\n```\n3\n```', 'easy', '入门,模拟', 500, 256, 'exact');

INSERT INTO test_cases (problem_id, order_no, input, expected_output) VALUES
(1, 1, '1 2\n', '3\n'),
(1, 2, '100 -200\n', '-100\n'),
(1, 3, '0 0\n', '0\n'),
(1, 4, '1000000000 1000000000\n', '2000000000\n');

-- ============================================================
-- 2. 两数之和（easy）
-- ============================================================
INSERT INTO problems (id, title, description, difficulty, tags, time_limit_ms, memory_limit_mb, judge_type) VALUES
(2, '两数之和', '给定一个整数数组 nums 和一个目标值 target，请你在该数组中找出和为目标值 target 的两个整数，并返回它们的下标。\n\n## 输入\n\n第一行两个整数 n 和 target，n 为数组长度（1 ≤ n ≤ 10^4）。\n第二行 n 个整数 nums[i]（-10^9 ≤ nums[i] ≤ 10^9）。\n保证存在唯一解，且同一个元素不能使用两次。\n\n## 输出\n\n两个用空格分隔的下标（较小的在前），保证存在且唯一。\n\n## 示例\n\n输入：\n```\n4 9\n2 7 11 15\n```\n输出：\n```\n0 1\n```', 'easy', '数组,哈希表', 500, 256, 'exact');

INSERT INTO test_cases (problem_id, order_no, input, expected_output) VALUES
(2, 1, '4 9\n2 7 11 15\n', '0 1\n'),
(2, 2, '3 6\n3 2 4\n', '1 2\n'),
(2, 3, '2 6\n3 3\n', '0 1\n'),
(2, 4, '5 10\n1 2 3 7 5\n', '2 4\n');

-- ============================================================
-- 3. 最长无重复子串（medium）
-- ============================================================
INSERT INTO problems (id, title, description, difficulty, tags, time_limit_ms, memory_limit_mb, judge_type) VALUES
(3, '最长无重复字符子串', '给定一个字符串 s，请你找出其中不含有重复字符的最长子串的长度。\n\n## 输入\n\n一个字符串 s（长度 1 ≤ |s| ≤ 10^5，仅含小写字母与数字）。\n\n## 输出\n\n一个整数，表示最长无重复字符子串的长度。\n\n## 示例\n\n输入：\n```\nabcabcbb\n```\n输出：\n```\n3\n```\n\n解释：最长子串为 "abc"，长度为 3。', 'medium', '字符串,滑动窗口', 500, 256, 'exact');

INSERT INTO test_cases (problem_id, order_no, input, expected_output) VALUES
(3, 1, 'abcabcbb\n', '3\n'),
(3, 2, 'bbbbb\n', '1\n'),
(3, 3, 'pwwkew\n', '3\n'),
(3, 4, 'a\n', '1\n'),
(3, 5, 'abcdefghijklmnopqrstuvwxyz0123456789\n', '36\n');

-- ============================================================
-- 4. 合并区间（medium）
-- ============================================================
INSERT INTO problems (id, title, description, difficulty, tags, time_limit_ms, memory_limit_mb, judge_type) VALUES
(4, '合并区间', '以数组 intervals 表示若干个区间的集合，其中单个区间为 [start, end]（start ≤ end）。请你合并所有重叠的区间，并返回一个不重叠的区间数组，该数组需恰好覆盖输入中的所有区间。\n\n## 输入\n\n第一行一个整数 n（1 ≤ n ≤ 10^4）。\n接下来 n 行，每行两个整数 start、end（-10^9 ≤ start ≤ end ≤ 10^9）。\n\n## 输出\n\n合并后的区间，每行一个，按 start 升序；每个区间形如 "start end"。\n\n## 示例\n\n输入：\n```\n4\n1 3\n2 6\n8 10\n15 18\n```\n输出：\n```\n1 6\n8 10\n15 18\n```', 'medium', '排序,区间', 500, 256, 'exact');

INSERT INTO test_cases (problem_id, order_no, input, expected_output) VALUES
(4, 1, '4\n1 3\n2 6\n8 10\n15 18\n', '1 6\n8 10\n15 18\n'),
(4, 2, '2\n1 4\n4 5\n', '1 5\n'),
(4, 3, '1\n1 1\n', '1 1\n'),
(4, 4, '3\n5 6\n1 2\n3 4\n', '1 2\n3 4\n5 6\n'),
(4, 5, '2\n-5 -1\n-3 0\n', '-5 0\n');

-- ============================================================
-- 5. 单链表反转（hard）
-- ============================================================
INSERT INTO problems (id, title, description, difficulty, tags, time_limit_ms, memory_limit_mb, judge_type) VALUES
(5, '反转链表', '给定单链表的头结点，反转该链表并返回反转后链表的头结点。\n\n## 输入\n\n第一行一个整数 n（0 ≤ n ≤ 10^5）。\n第二行 n 个整数，为链表各结点值（-10^9 ≤ val ≤ 10^9）。\n\n## 输出\n\n反转后链表各结点值，用空格分隔；空链表输出空行。\n\n## 示例\n\n输入：\n```\n5\n1 2 3 4 5\n```\n输出：\n```\n5 4 3 2 1\n```', 'hard', '链表,递归', 500, 256, 'exact');

INSERT INTO test_cases (problem_id, order_no, input, expected_output) VALUES
(5, 1, '5\n1 2 3 4 5\n', '5 4 3 2 1\n'),
(5, 2, '1\n42\n', '42\n'),
(5, 3, '0\n', '\n'),
(5, 4, '3\n-1 0 1\n', '1 0 -1\n'),
(5, 5, '6\n9 8 7 6 5 4\n', '4 5 6 7 8 9\n');
